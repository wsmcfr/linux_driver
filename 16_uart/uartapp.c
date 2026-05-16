#define _DEFAULT_SOURCE                 /* 使 glibc 暴露 cfmakeraw、usleep 等常用 POSIX 扩展接口。 */

#include <ctype.h>                      /* 提供 isprint、isxdigit 等字符判断接口，用于显示和解析转义字符串。 */
#include <errno.h>                      /* 提供 errno，系统调用失败后用它判断具体错误原因。 */
#include <fcntl.h>                      /* 提供 open 函数和 O_RDWR、O_NOCTTY 等文件打开标志。 */
#include <signal.h>                     /* 提供 signal 和 sig_atomic_t，用于 Ctrl+C 安全退出循环。 */
#include <stdbool.h>                    /* 提供 bool、true、false，让状态变量含义更直接。 */
#include <stdint.h>                     /* 提供 uint8_t 等固定宽度整数类型，用于明确字节数据。 */
#include <stdio.h>                      /* 提供 printf、fprintf、perror 等标准输入输出接口。 */
#include <stdlib.h>                     /* 提供 malloc、free、strtol、EXIT_SUCCESS、EXIT_FAILURE 等接口。 */
#include <string.h>                     /* 提供 strcmp、strlen、memcpy、strerror 等字符串和内存操作接口。 */
#include <sys/select.h>                 /* 提供 select、fd_set，用于同时等待串口和键盘输入。 */
#include <sys/time.h>                   /* 提供 struct timeval，用于 select 超时和周期发送等待。 */
#include <termios.h>                    /* 提供 termios 串口配置接口，这是 Linux 用户态配置 UART 的核心头文件。 */
#include <unistd.h>                     /* 提供 read、write、close、STDIN_FILENO 等 POSIX 系统调用接口。 */

#define RX_BUFFER_SIZE      256         /* 每次从串口读取的缓冲区大小；测试工具按块显示，不要求一次读完整协议帧。 */
#define MIN_LOOP_INTERVAL   1           /* 周期发送的最小间隔，单位毫秒，避免用户传 0 导致疯狂占用 CPU。 */
#define MAX_LOOP_INTERVAL   600000      /* 周期发送的最大间隔，单位毫秒，这里限制为 10 分钟防止参数误输。 */

/*
 * g_stop - Ctrl+C 退出标志。
 * 这个变量会在信号处理函数中写入，在 recv/loop/term 主循环中读取。
 * sig_atomic_t 是 C 标准保证可在信号处理函数中安全读写的整数类型。
 */
static volatile sig_atomic_t g_stop;

/*
 * struct tx_buffer - 保存待发送数据的二进制缓冲区。
 * @data: 指向 malloc 申请的字节数组，内容已经完成 \r、\n、\xHH 等转义解析。
 * @len: data 中有效字节数量，允许包含 0x00，所以不能用 strlen 判断长度。
 */
struct tx_buffer {
	uint8_t *data;                       /* 保存真正要写入串口的字节数据。 */
	size_t len;                          /* 保存待发送字节数。 */
};

/*
 * show_usage - 打印串口测试工具的使用说明。
 * @progname: 当前程序名，通常来自 argv[0]。
 *
 * 主要流程：
 * 1. 说明串口设备、波特率和命令格式。
 * 2. 给出接收、单次发送、周期发送、终端透传四种常用示例。
 *
 * 返回值：无；该函数只负责打印帮助文本。
 */
static void show_usage(const char *progname)
{
	printf("Usage:\n");                                                        /* 打印用法标题。 */
	printf("  %s -h\n", progname);                                             /* 打印帮助命令。 */
	printf("  %s <dev> <baud> recv\n", progname);                              /* 打印只接收模式命令。 */
	printf("  %s <dev> <baud> send <text>\n", progname);                       /* 打印单次发送模式命令。 */
	printf("  %s <dev> <baud> loop <text> <interval_ms>\n", progname);         /* 打印周期发送模式命令。 */
	printf("  %s <dev> <baud> term\n", progname);                              /* 打印终端透传模式命令。 */
	printf("\nExamples:\n");                                                    /* 打印示例标题。 */
	printf("  %s /dev/ttySTM2 115200 recv\n", progname);                       /* 示例：监听 F4 发来的数据。 */
	printf("  %s /dev/ttySTM2 115200 send \"hello\\r\\n\"\n", progname);        /* 示例：主动发送一行文本。 */
	printf("  %s /dev/ttySTM2 115200 loop \"ping\\r\\n\" 1000\n", progname);    /* 示例：每 1000ms 发送一次 ping。 */
	printf("  %s /dev/ttySTM2 115200 term\n", progname);                       /* 示例：进入键盘输入和串口接收共存的模式。 */
	printf("\nEscape support in <text>: \\\\ \\r \\n \\t \\xHH\n");             /* 说明发送字符串支持的常见转义写法。 */
}

/*
 * handle_signal - 处理 Ctrl+C 等退出信号。
 * @signo: 内核传入的信号编号，本程序不需要区分具体编号。
 *
 * 主要流程：
 * 1. 只设置全局退出标志。
 * 2. 不在信号处理函数里调用 printf/free/close 等非异步信号安全接口。
 *
 * 返回值：无；循环会在下一次检查 g_stop 时自然退出。
 */
static void handle_signal(int signo)
{
	(void)signo;                         /* 明确告诉编译器这个参数当前不需要使用，避免告警。 */
	g_stop = 1;                          /* 设置退出标志，让主循环从阻塞等待中被 EINTR 打断后退出。 */
}

/*
 * baud_to_speed - 把用户输入的整数波特率转换成 termios 使用的 speed_t 常量。
 * @baud: 用户输入的整数波特率，例如 115200。
 * @speed: 输出参数，成功时写入 B115200 这类 termios 常量。
 *
 * 主要流程：
 * 1. switch 匹配 Linux 常见波特率。
 * 2. 对部分平台可能没有定义的高速波特率使用 #ifdef 保护。
 *
 * 返回值：
 * 0 表示转换成功；
 * -1 表示波特率不在当前程序支持范围内。
 */
static int baud_to_speed(int baud, speed_t *speed)
{
	switch (baud) {                      /* 按用户输入的整数波特率选择对应 termios 常量。 */
	case 9600:                           /* 9600 是低速调试中常见波特率。 */
		*speed = B9600;                  /* 写入 termios 的 9600 常量。 */
		return 0;                        /* 返回成功。 */
	case 19200:                          /* 19200 是部分老设备常用波特率。 */
		*speed = B19200;                 /* 写入 termios 的 19200 常量。 */
		return 0;                        /* 返回成功。 */
	case 38400:                          /* 38400 常用于普通串口调试。 */
		*speed = B38400;                 /* 写入 termios 的 38400 常量。 */
		return 0;                        /* 返回成功。 */
	case 57600:                          /* 57600 是中等速率串口常见配置。 */
		*speed = B57600;                 /* 写入 termios 的 57600 常量。 */
		return 0;                        /* 返回成功。 */
	case 115200:                         /* 115200 是本项目 RS485 调试默认推荐波特率。 */
		*speed = B115200;                /* 写入 termios 的 115200 常量。 */
		return 0;                        /* 返回成功。 */
#ifdef B230400
	case 230400:                         /* 230400 用于链路稳定后提高通信速率。 */
		*speed = B230400;                /* 写入 termios 的 230400 常量。 */
		return 0;                        /* 返回成功。 */
#endif
#ifdef B460800
	case 460800:                         /* 460800 用于较高速串口通信，硬件和线缆必须可靠。 */
		*speed = B460800;                /* 写入 termios 的 460800 常量。 */
		return 0;                        /* 返回成功。 */
#endif
#ifdef B921600
	case 921600:                         /* 921600 对 RS485 线缆、终端匹配和模块质量要求更高。 */
		*speed = B921600;                /* 写入 termios 的 921600 常量。 */
		return 0;                        /* 返回成功。 */
#endif
	default:                             /* 没有匹配到支持的波特率。 */
		return -1;                       /* 返回失败，让上层打印清晰的参数错误。 */
	}
}

/*
 * parse_int_range - 解析带上下限的十进制整数。
 * @text: 用户输入的字符串。
 * @min_value: 允许的最小值。
 * @max_value: 允许的最大值。
 * @out_value: 输出参数，成功时保存解析后的整数。
 *
 * 主要流程：
 * 1. 使用 strtol 而不是 atoi，因为 strtol 可以检查非法字符和溢出。
 * 2. 检查字符串必须被完整消费，避免 "115200abc" 被误接受。
 * 3. 检查上下限，避免异常参数进入串口配置或循环等待。
 *
 * 返回值：
 * 0 表示解析成功；
 * -1 表示输入为空、含非法字符、溢出或超出范围。
 */
static int parse_int_range(const char *text, int min_value, int max_value, int *out_value)
{
	char *endptr;                        /* strtol 会把未解析到的位置写到这个指针里。 */
	long value;                          /* strtol 返回 long，便于先检查范围再转成 int。 */

	if (text == NULL || *text == '\0') {  /* 空指针或空字符串都不是合法整数。 */
		return -1;                       /* 返回失败，交给调用者打印参数错误。 */
	}

	errno = 0;                            /* 调用 strtol 前清 errno，便于判断 ERANGE。 */
	value = strtol(text, &endptr, 10);    /* 按十进制解析用户输入。 */
	if (errno != 0 || *endptr != '\0') {  /* errno 非 0 表示溢出等错误，endptr 未到结尾表示有非法字符。 */
		return -1;                       /* 返回失败。 */
	}

	if (value < min_value || value > max_value) { /* 检查解析值是否落在调用者要求的范围内。 */
		return -1;                       /* 超出范围时返回失败。 */
	}

	*out_value = (int)value;             /* 范围确认安全后再转换成 int 输出。 */
	return 0;                            /* 返回成功。 */
}

/*
 * hex_value - 把一个十六进制字符转换成 0~15 的数值。
 * @ch: 待转换字符，可以是 0-9、a-f、A-F。
 *
 * 主要流程：
 * 1. 分别处理数字、小写字母和大写字母。
 * 2. 非十六进制字符返回 -1。
 *
 * 返回值：
 * 0~15 表示合法十六进制值；
 * -1 表示非法字符。
 */
static int hex_value(char ch)
{
	if (ch >= '0' && ch <= '9')           /* 数字字符直接减 '0' 得到数值。 */
		return ch - '0';                 /* 返回 0 到 9。 */
	if (ch >= 'a' && ch <= 'f')           /* 小写 a-f 表示 10 到 15。 */
		return ch - 'a' + 10;            /* 返回 10 到 15。 */
	if (ch >= 'A' && ch <= 'F')           /* 大写 A-F 表示 10 到 15。 */
		return ch - 'A' + 10;            /* 返回 10 到 15。 */
	return -1;                            /* 其他字符不是十六进制字符。 */
}

/*
 * parse_tx_text - 解析命令行中的待发送字符串。
 * @text: 用户输入的字符串，支持 \r、\n、\t、\\、\xHH 等转义。
 * @out: 输出参数，成功时保存 malloc 得到的发送缓冲区。
 *
 * 主要流程：
 * 1. 按原始字符串长度申请缓冲区，因为转义解析后长度只会小于或等于原长度。
 * 2. 逐字节扫描输入，遇到反斜杠时解析常用转义。
 * 3. 对 \xHH 做严格检查，避免用户误以为发送了某个十六进制字节。
 *
 * 返回值：
 * 0 表示解析成功，调用者后续必须 free(out->data)；
 * -1 表示内存申请失败或转义格式错误。
 */
static int parse_tx_text(const char *text, struct tx_buffer *out)
{
	size_t in_len;                        /* 保存输入字符串长度。 */
	size_t i;                             /* 输入字符串扫描下标。 */
	size_t j = 0;                         /* 输出缓冲区写入下标。 */
	uint8_t *data;                        /* 临时保存申请到的输出缓冲区。 */

	if (text == NULL || out == NULL) {    /* 参数为空说明调用者使用错误。 */
		errno = EINVAL;                   /* 设置 EINVAL，方便上层 perror 打印。 */
		return -1;                        /* 返回失败。 */
	}

	in_len = strlen(text);                /* 统计输入字符串长度，命令行参数本身不能包含 NUL。 */
	data = malloc(in_len + 1);            /* 申请输出缓冲区，多 1 字节只是留作安全余量。 */
	if (data == NULL) {                   /* malloc 失败通常是内存不足。 */
		return -1;                        /* 返回失败，errno 由 malloc 设置。 */
	}

	for (i = 0; i < in_len; i++) {        /* 逐字符扫描输入文本。 */
		if (text[i] != '\\') {            /* 普通字符不需要特殊处理。 */
			data[j++] = (uint8_t)text[i]; /* 直接写入输出缓冲区。 */
			continue;                     /* 继续扫描下一个字符。 */
		}

		i++;                              /* 跳过反斜杠，查看后面的转义字符。 */
		if (i >= in_len) {                /* 反斜杠出现在字符串末尾，说明转义不完整。 */
			fprintf(stderr, "bad escape: trailing backslash\n"); /* 打印清晰错误。 */
			free(data);                   /* 释放已经申请的缓冲区，避免泄漏。 */
			return -1;                    /* 返回失败。 */
		}

		switch (text[i]) {                /* 根据反斜杠后的字符决定写入什么字节。 */
		case 'r':                         /* \r 表示回车，很多串口协议用它作为行结束的一部分。 */
			data[j++] = '\r';             /* 写入 CR，十六进制 0x0D。 */
			break;                        /* 当前转义处理完成。 */
		case 'n':                         /* \n 表示换行，常和 \r 组合成 \r\n。 */
			data[j++] = '\n';             /* 写入 LF，十六进制 0x0A。 */
			break;                        /* 当前转义处理完成。 */
		case 't':                         /* \t 表示水平制表符，偶尔用于调试文本分隔。 */
			data[j++] = '\t';             /* 写入 TAB，十六进制 0x09。 */
			break;                        /* 当前转义处理完成。 */
		case '\\':                        /* \\ 表示发送一个真正的反斜杠字符。 */
			data[j++] = '\\';             /* 写入反斜杠。 */
			break;                        /* 当前转义处理完成。 */
		case 'x': {                       /* \xHH 表示按十六进制发送一个任意字节。 */
			int high;                     /* 保存高 4 位十六进制值。 */
			int low;                      /* 保存低 4 位十六进制值。 */

			if (i + 2 >= in_len) {        /* \x 后面必须再跟两个十六进制字符。 */
				fprintf(stderr, "bad escape: \\x needs two hex digits\n"); /* 打印格式错误。 */
				free(data);               /* 释放缓冲区。 */
				return -1;                /* 返回失败。 */
			}

			high = hex_value(text[i + 1]); /* 转换高 4 位字符。 */
			low = hex_value(text[i + 2]);  /* 转换低 4 位字符。 */
			if (high < 0 || low < 0) {     /* 任意一位非法都不能继续。 */
				fprintf(stderr, "bad escape: invalid hex digit in \\x%c%c\n", text[i + 1], text[i + 2]); /* 打印错误位置。 */
				free(data);               /* 释放缓冲区。 */
				return -1;                /* 返回失败。 */
			}

			data[j++] = (uint8_t)((high << 4) | low); /* 合成一个字节并写入输出缓冲区。 */
			i += 2;                       /* 跳过已经消费的两个十六进制字符。 */
			break;                        /* 当前转义处理完成。 */
		}
		default:                          /* 未知转义不直接报错，按常见命令行习惯发送转义后的字符本身。 */
			data[j++] = (uint8_t)text[i]; /* 例如 \a 会发送字符 a，避免过度限制调试输入。 */
			break;                        /* 当前转义处理完成。 */
		}
	}

	out->data = data;                     /* 把解析后的缓冲区交给调用者管理。 */
	out->len = j;                         /* 保存解析后的真实字节数。 */
	return 0;                             /* 返回成功。 */
}

/*
 * configure_serial - 配置 Linux 串口为 8N1 原始模式。
 * @fd: 已经 open 成功的串口文件描述符。
 * @baud: 用户指定的整数波特率。
 *
 * 主要流程：
 * 1. tcgetattr 读取原始配置。
 * 2. cfmakeraw 关闭规范模式、回显、特殊字符处理等终端行为。
 * 3. 配置 8 数据位、无校验、1 停止位、关闭软硬件流控。
 * 4. tcflush 清掉旧数据，再用 tcsetattr 写回配置。
 *
 * 返回值：
 * 0 表示配置成功；
 * -1 表示读取配置、波特率转换或写回配置失败。
 */
static int configure_serial(int fd, int baud)
{
	struct termios tio;                   /* 保存串口属性配置。 */
	speed_t speed;                        /* 保存 termios 使用的波特率常量。 */

	if (baud_to_speed(baud, &speed) < 0) { /* 把整数波特率转换成 termios 常量。 */
		fprintf(stderr, "unsupported baud: %d\n", baud); /* 打印不支持的波特率。 */
		errno = EINVAL;                   /* 设置参数错误，便于调用者识别。 */
		return -1;                        /* 返回失败。 */
	}

	if (tcgetattr(fd, &tio) < 0) {        /* 读取当前串口配置，后面在这个基础上修改。 */
		perror("tcgetattr");             /* 打印系统调用失败原因。 */
		return -1;                        /* 返回失败。 */
	}

	cfmakeraw(&tio);                      /* 进入原始模式，避免 Linux 终端层改写串口数据。 */
	cfsetispeed(&tio, speed);             /* 设置输入波特率。 */
	cfsetospeed(&tio, speed);             /* 设置输出波特率。 */

	tio.c_cflag |= CLOCAL;                /* 忽略 modem 控制线，普通 UART/USB 串口调试必须打开。 */
	tio.c_cflag |= CREAD;                 /* 允许接收数据，否则 read 可能读不到串口输入。 */
	tio.c_cflag &= ~CSIZE;                /* 先清空数据位掩码，避免旧配置残留。 */
	tio.c_cflag |= CS8;                   /* 设置 8 个数据位。 */
	tio.c_cflag &= ~PARENB;               /* 关闭奇偶校验，对应 8N1 中的 N。 */
	tio.c_cflag &= ~CSTOPB;               /* 使用 1 个停止位，对应 8N1 中的 1。 */
#ifdef CRTSCTS
	tio.c_cflag &= ~CRTSCTS;              /* 关闭 RTS/CTS 硬件流控，避免没有接流控线时卡住发送。 */
#endif
	tio.c_iflag &= ~(IXON | IXOFF | IXANY); /* 关闭 XON/XOFF 软件流控，避免 0x11/0x13 被终端层吞掉。 */
	tio.c_cc[VMIN] = 0;                   /* read 最少读 0 字节即可返回，实际等待由 select 控制。 */
	tio.c_cc[VTIME] = 1;                  /* read 最长等待 0.1 秒，防止异常情况下永久阻塞。 */

	if (tcflush(fd, TCIOFLUSH) < 0) {     /* 清理配置前残留的输入输出数据，避免旧字节干扰测试。 */
		perror("tcflush");               /* 打印清理失败原因。 */
		return -1;                        /* 返回失败。 */
	}

	if (tcsetattr(fd, TCSANOW, &tio) < 0) { /* 立即把新配置写回串口驱动。 */
		perror("tcsetattr");             /* 打印配置失败原因。 */
		return -1;                        /* 返回失败。 */
	}

	return 0;                             /* 所有配置成功。 */
}

/*
 * open_serial - 打开并配置串口设备。
 * @devname: 串口设备节点路径，例如 /dev/ttySTM2。
 * @baud: 用户指定的整数波特率。
 *
 * 主要流程：
 * 1. 用 O_RDWR 打开串口，因为测试工具需要同时收发。
 * 2. 用 O_NOCTTY 避免串口成为当前进程控制终端。
 * 3. 调用 configure_serial 设置 8N1 原始模式。
 *
 * 返回值：
 * 成功时返回非负文件描述符；
 * 失败时返回 -1，并已打印错误原因。
 */
static int open_serial(const char *devname, int baud)
{
	int fd;                               /* 保存 open 返回的文件描述符。 */
	int flags = O_RDWR | O_NOCTTY;        /* 读写打开串口，并禁止它成为控制终端。 */

#ifdef O_CLOEXEC
	flags |= O_CLOEXEC;                   /* 子进程 exec 时自动关闭 fd，避免资源泄漏到外部程序。 */
#endif

	fd = open(devname, flags);            /* 打开用户指定的串口设备节点。 */
	if (fd < 0) {                         /* 打开失败通常是设备名错误、权限不足或驱动未加载。 */
		fprintf(stderr, "open %s failed: %s\n", devname, strerror(errno)); /* 打印具体失败原因。 */
		return -1;                        /* 返回失败。 */
	}

	if (configure_serial(fd, baud) < 0) { /* 配置串口参数。 */
		close(fd);                        /* 配置失败时关闭已经打开的设备，避免 fd 泄漏。 */
		return -1;                        /* 返回失败。 */
	}

	return fd;                            /* 返回已经配置好的串口文件描述符。 */
}

/*
 * print_bytes - 按 TEXT 和 HEX 两种形式显示一段接收或发送数据。
 * @tag: 数据方向标签，例如 "RX" 或 "TX"。
 * @data: 待显示的字节数组。
 * @len: data 中有效字节数量。
 *
 * 主要流程：
 * 1. TEXT 区域显示可打印 ASCII 字符。
 * 2. 对 \r、\n、\t 单独显示转义名，避免换行把调试输出冲乱。
 * 3. HEX 区域显示每个字节的十六进制值，方便检查协议帧和不可见字符。
 *
 * 返回值：无；该函数只打印调试信息。
 */
static void print_bytes(const char *tag, const uint8_t *data, size_t len)
{
	size_t i;                             /* 循环变量，用于遍历每个字节。 */

	printf("[%s %zu] TEXT: ", tag, len);  /* 打印方向和字节数，方便观察是否丢包或粘包。 */
	for (i = 0; i < len; i++) {           /* 遍历所有字节并显示文本形式。 */
		if (data[i] == '\r')              /* 回车是不可见控制字符。 */
			printf("\\r");               /* 显示为 \r，避免光标回到行首影响观察。 */
		else if (data[i] == '\n')         /* 换行也是控制字符。 */
			printf("\\n");               /* 显示为 \n，避免调试行被拆开。 */
		else if (data[i] == '\t')         /* TAB 会改变列位置。 */
			printf("\\t");               /* 显示为 \t，保持输出整齐。 */
		else if (isprint(data[i]))        /* 可打印字符可以直接显示。 */
			putchar(data[i]);             /* 输出原字符。 */
		else                              /* 其他不可打印字节例如 0x00、0xFF。 */
			putchar('.');                 /* 用点号占位，详细值看后面的 HEX 区域。 */
	}

	printf(" | HEX:");                    /* 分隔文本显示和十六进制显示。 */
	for (i = 0; i < len; i++)             /* 遍历所有字节并显示十六进制。 */
		printf(" %02X", data[i]);        /* 每个字节固定两位大写十六进制，便于和协议文档对照。 */
	printf("\n");                         /* 一次接收或发送块显示完毕后换行。 */
	fflush(stdout);                       /* 立即刷新输出，方便串口实时调试。 */
}

/*
 * write_all - 保证把一段数据完整写入串口。
 * @fd: 串口文件描述符。
 * @data: 待发送字节数组。
 * @len: 待发送字节数量。
 *
 * 主要流程：
 * 1. write 可能只写入部分数据，因此使用循环补齐剩余字节。
 * 2. EINTR 表示被信号打断，可以继续写。
 * 3. 其他错误直接返回失败。
 *
 * 返回值：
 * 0 表示全部写入成功；
 * -1 表示写入失败。
 */
static int write_all(int fd, const uint8_t *data, size_t len)
{
	size_t done = 0;                      /* 记录已经写入的字节数。 */

	while (done < len) {                  /* 只要还有剩余字节就继续写。 */
		ssize_t ret = write(fd, data + done, len - done); /* 尝试写入剩余数据。 */

		if (ret < 0) {                    /* write 返回负数表示失败。 */
			if (errno == EINTR)           /* EINTR 表示被信号打断，不代表串口错误。 */
				continue;                 /* 继续写剩余数据。 */
			perror("write");             /* 打印真实写入错误。 */
			return -1;                    /* 返回失败。 */
		}

		if (ret == 0) {                   /* 正常串口 write 不应该返回 0。 */
			fprintf(stderr, "write returned 0 bytes\n"); /* 打印异常情况。 */
			errno = EIO;                  /* 设置 I/O 错误，表示链路状态异常。 */
			return -1;                    /* 返回失败，避免死循环。 */
		}

		done += (size_t)ret;              /* 累加本次实际写入字节数。 */
	}

	if (tcdrain(fd) < 0) {                /* 等待内核把输出队列里的数据真正发完。 */
		perror("tcdrain");               /* 打印等待发送完成失败原因。 */
		return -1;                        /* 返回失败。 */
	}

	return 0;                             /* 全部字节成功写入并排空。 */
}

/*
 * wait_readable - 等待某个文件描述符变为可读。
 * @fd: 要等待的文件描述符。
 * @timeout_ms: 超时时间，单位毫秒；负数表示无限等待。
 *
 * 主要流程：
 * 1. 用 select 等待 fd 可读。
 * 2. EINTR 时返回 0，让上层循环检查 g_stop。
 * 3. 超时返回 0，真正可读返回 1。
 *
 * 返回值：
 * 1 表示 fd 可读；
 * 0 表示超时或被信号打断；
 * -1 表示 select 失败。
 */
static int wait_readable(int fd, int timeout_ms)
{
	fd_set rfds;                          /* select 使用的读集合。 */
	struct timeval tv;                    /* select 使用的超时结构体。 */
	struct timeval *tvp = NULL;           /* 传给 select 的超时指针，NULL 表示无限等待。 */
	int ret;                              /* 保存 select 返回值。 */

	FD_ZERO(&rfds);                       /* 清空读集合。 */
	FD_SET(fd, &rfds);                    /* 把目标 fd 加入读集合。 */

	if (timeout_ms >= 0) {                /* 非负超时表示需要定时返回。 */
		tv.tv_sec = timeout_ms / 1000;    /* 毫秒转换为秒部分。 */
		tv.tv_usec = (timeout_ms % 1000) * 1000; /* 毫秒余数转换为微秒部分。 */
		tvp = &tv;                        /* 使用这个超时结构体。 */
	}

	ret = select(fd + 1, &rfds, NULL, NULL, tvp); /* 等待 fd 可读或超时。 */
	if (ret < 0) {                         /* select 返回负数表示失败。 */
		if (errno == EINTR)               /* EINTR 表示被 Ctrl+C 等信号打断。 */
			return 0;                     /* 返回 0，让上层检查退出标志。 */
		perror("select");                /* 打印真实 select 错误。 */
		return -1;                        /* 返回失败。 */
	}

	if (ret == 0)                          /* ret 为 0 表示等待超时。 */
		return 0;                         /* 返回超时。 */

	return FD_ISSET(fd, &rfds) ? 1 : 0;    /* 正常情况下 fd 在集合中则表示可读。 */
}

/*
 * receive_loop - 持续接收串口数据并打印。
 * @fd: 已配置好的串口文件描述符。
 *
 * 主要流程：
 * 1. 注册 Ctrl+C 处理函数。
 * 2. 使用 select 等待串口可读。
 * 3. read 读到数据后同时打印 TEXT 和 HEX。
 *
 * 返回值：
 * EXIT_SUCCESS 表示用户正常 Ctrl+C 退出；
 * EXIT_FAILURE 表示串口读取失败。
 */
static int receive_loop(int fd)
{
	uint8_t buffer[RX_BUFFER_SIZE];        /* 保存每次 read 读到的串口数据。 */

	g_stop = 0;                            /* 每次进入循环前清除旧退出标志。 */
	signal(SIGINT, handle_signal);         /* 捕获 Ctrl+C，让程序能打印退出信息并关闭 fd。 */
	printf("Receiving, press Ctrl+C to stop.\n"); /* 提示用户当前处于接收模式。 */

	while (!g_stop) {                      /* 持续运行直到用户按 Ctrl+C。 */
		int ready = wait_readable(fd, 1000); /* 每秒醒来一次，便于检查退出标志。 */

		if (ready < 0)                    /* select 失败说明等待逻辑出错。 */
			return EXIT_FAILURE;          /* 返回失败。 */
		if (ready == 0)                   /* 超时或被信号打断时不读数据。 */
			continue;                     /* 继续下一轮等待。 */

		while (1) {                       /* 串口可读后尽量把当前已到达的数据读干净。 */
			ssize_t len = read(fd, buffer, sizeof(buffer)); /* 从串口读取一块数据。 */

			if (len < 0) {                /* read 失败。 */
				if (errno == EINTR)       /* 被信号打断时不是串口错误。 */
					break;                /* 跳出内层读取，外层检查 g_stop。 */
				if (errno == EAGAIN || errno == EWOULDBLOCK) /* 暂时无数据时结束本轮读取。 */
					break;                /* 回到 select 等待下一批数据。 */
				perror("read");          /* 打印真实读取错误。 */
				return EXIT_FAILURE;      /* 返回失败。 */
			}

			if (len == 0)                 /* VMIN=0/VTIME=1 时可能出现 0 字节返回。 */
				break;                    /* 当前没有更多数据，回到 select。 */

			print_bytes("RX", buffer, (size_t)len); /* 打印收到的数据。 */

			if ((size_t)len < sizeof(buffer)) /* 如果这次没填满缓冲区，通常说明当前批次读完了。 */
				break;                    /* 回到 select 等待下一次可读。 */
		}
	}

	printf("Receive stopped.\n");         /* 提示用户接收循环已退出。 */
	return EXIT_SUCCESS;                  /* Ctrl+C 属于正常退出。 */
}

/*
 * send_once - 向串口发送一次数据。
 * @fd: 已配置好的串口文件描述符。
 * @tx: 已解析好的待发送数据。
 *
 * 主要流程：
 * 1. 检查发送长度不能为 0。
 * 2. 调用 write_all 完整发送。
 * 3. 打印 TX 的 TEXT 和 HEX，方便和 F4 接收日志对照。
 *
 * 返回值：
 * EXIT_SUCCESS 表示发送成功；
 * EXIT_FAILURE 表示发送失败。
 */
static int send_once(int fd, const struct tx_buffer *tx)
{
	if (tx->len == 0) {                    /* 空数据没有调试意义，也容易让用户误以为发送成功。 */
		fprintf(stderr, "empty text is not allowed\n"); /* 打印明确错误。 */
		return EXIT_FAILURE;              /* 返回失败。 */
	}

	if (write_all(fd, tx->data, tx->len) < 0) /* 完整写入串口并等待发送完成。 */
		return EXIT_FAILURE;              /* 写入失败时返回失败。 */

	print_bytes("TX", tx->data, tx->len);  /* 打印发送出去的数据，方便检查 \r\n 和 \xHH。 */
	return EXIT_SUCCESS;                  /* 返回成功。 */
}

/*
 * sleep_ms_interruptible - 可被 Ctrl+C 打断的毫秒级等待。
 * @ms: 等待时间，单位毫秒。
 *
 * 主要流程：
 * 1. 使用 select 的 timeout 作为纯等待。
 * 2. 如果被信号打断，直接返回，让上层检查 g_stop。
 *
 * 返回值：无；该函数只负责节流等待。
 */
static void sleep_ms_interruptible(int ms)
{
	struct timeval tv;                    /* 保存 select 的等待时间。 */

	tv.tv_sec = ms / 1000;                /* 毫秒转换为秒部分。 */
	tv.tv_usec = (ms % 1000) * 1000;      /* 毫秒余数转换为微秒部分。 */
	(void)select(0, NULL, NULL, NULL, &tv); /* nfds 为 0 时 select 只做定时等待。 */
}

/*
 * loop_send - 按固定周期重复发送同一段数据。
 * @fd: 已配置好的串口文件描述符。
 * @tx: 已解析好的待发送数据。
 * @interval_ms: 发送间隔，单位毫秒。
 *
 * 主要流程：
 * 1. 注册 Ctrl+C 退出。
 * 2. 每轮调用 send_once 发送并打印。
 * 3. 发送后等待 interval_ms，再进入下一轮。
 *
 * 返回值：
 * EXIT_SUCCESS 表示用户正常 Ctrl+C 退出；
 * EXIT_FAILURE 表示发送失败。
 */
static int loop_send(int fd, const struct tx_buffer *tx, int interval_ms)
{
	unsigned long count = 0;              /* 记录已经发送的次数，便于观察循环是否持续运行。 */

	g_stop = 0;                            /* 清除旧退出标志。 */
	signal(SIGINT, handle_signal);         /* 捕获 Ctrl+C。 */
	printf("Loop sending every %d ms, press Ctrl+C to stop.\n", interval_ms); /* 提示当前周期发送参数。 */

	while (!g_stop) {                      /* 持续发送直到用户中断。 */
		printf("#%lu ", ++count);         /* 打印当前发送序号。 */
		if (send_once(fd, tx) != EXIT_SUCCESS) /* 发送一次测试数据。 */
			return EXIT_FAILURE;          /* 任意一次发送失败都退出，避免掩盖链路问题。 */
		sleep_ms_interruptible(interval_ms); /* 等待下一个周期。 */
	}

	printf("Loop sending stopped.\n");     /* 提示周期发送已停止。 */
	return EXIT_SUCCESS;                  /* Ctrl+C 属于正常退出。 */
}

/*
 * terminal_loop - 同时处理键盘输入和串口接收。
 * @fd: 已配置好的串口文件描述符。
 *
 * 主要流程：
 * 1. select 同时监听 STDIN 和串口 fd。
 * 2. 键盘输入一行后发送给串口。
 * 3. 串口收到数据后打印 RX 的 TEXT 和 HEX。
 *
 * 返回值：
 * EXIT_SUCCESS 表示用户正常 Ctrl+C 或 stdin 结束；
 * EXIT_FAILURE 表示 select/read/write 失败。
 */
static int terminal_loop(int fd)
{
	uint8_t rxbuf[RX_BUFFER_SIZE];         /* 保存串口接收数据。 */
	uint8_t txbuf[RX_BUFFER_SIZE];         /* 保存从键盘读到的待发送数据。 */

	g_stop = 0;                            /* 清除旧退出标志。 */
	signal(SIGINT, handle_signal);         /* 捕获 Ctrl+C。 */
	printf("Terminal mode. Type text then Enter to send, press Ctrl+C to stop.\n"); /* 提示终端模式操作方式。 */

	while (!g_stop) {                      /* 持续运行直到用户中断。 */
		fd_set rfds;                      /* select 使用的读集合。 */
		int maxfd;                        /* 保存 select 需要的最大 fd + 1 的基础值。 */
		int ret;                          /* 保存 select 返回值。 */

		FD_ZERO(&rfds);                   /* 清空读集合。 */
		FD_SET(fd, &rfds);                /* 监听串口输入。 */
		FD_SET(STDIN_FILENO, &rfds);      /* 监听键盘输入。 */
		maxfd = fd > STDIN_FILENO ? fd : STDIN_FILENO; /* 计算最大 fd。 */

		ret = select(maxfd + 1, &rfds, NULL, NULL, NULL); /* 阻塞等待任一输入源可读。 */
		if (ret < 0) {                    /* select 失败。 */
			if (errno == EINTR)           /* Ctrl+C 会打断 select。 */
				continue;                 /* 回到循环顶部检查 g_stop。 */
			perror("select");            /* 打印真实 select 错误。 */
			return EXIT_FAILURE;          /* 返回失败。 */
		}

		if (FD_ISSET(fd, &rfds)) {        /* 串口有数据可读。 */
			ssize_t len = read(fd, rxbuf, sizeof(rxbuf)); /* 读取串口数据。 */

			if (len < 0) {                /* read 失败。 */
				if (errno == EINTR)       /* 信号打断时继续。 */
					continue;             /* 回到循环。 */
				perror("read serial");   /* 打印串口读取错误。 */
				return EXIT_FAILURE;      /* 返回失败。 */
			}
			if (len > 0)                 /* len 为 0 表示暂时没有实际数据。 */
				print_bytes("RX", rxbuf, (size_t)len); /* 打印接收数据。 */
		}

		if (FD_ISSET(STDIN_FILENO, &rfds)) { /* 键盘有一行输入可读。 */
			ssize_t len = read(STDIN_FILENO, txbuf, sizeof(txbuf)); /* 读取键盘输入，默认终端规范模式下一般以 Enter 结束。 */

			if (len < 0) {                /* stdin 读取失败。 */
				if (errno == EINTR)       /* 信号打断时继续。 */
					continue;             /* 回到循环。 */
				perror("read stdin");    /* 打印键盘读取错误。 */
				return EXIT_FAILURE;      /* 返回失败。 */
			}
			if (len == 0) {               /* stdin EOF，常见于管道输入结束。 */
				printf("stdin closed.\n"); /* 提示标准输入关闭。 */
				return EXIT_SUCCESS;      /* 正常退出。 */
			}

			if (write_all(fd, txbuf, (size_t)len) < 0) /* 把键盘输入原样发给串口。 */
				return EXIT_FAILURE;      /* 发送失败时退出。 */
			print_bytes("TX", txbuf, (size_t)len); /* 打印发送内容，方便观察是否带了 \n。 */
		}
	}

	printf("Terminal stopped.\n");         /* 提示终端模式已退出。 */
	return EXIT_SUCCESS;                  /* Ctrl+C 属于正常退出。 */
}

/*
 * main - Linux UART/RS485 用户态收发测试程序入口。
 * @argc: 命令行参数数量。
 * @argv: 命令行参数数组，包含设备节点、波特率、命令和命令参数。
 *
 * 主要流程：
 * 1. 解析 -h 和基础参数。
 * 2. 打开并配置串口为 115200 8N1 这类原始模式。
 * 3. 按 recv/send/loop/term 四种命令进入对应测试路径。
 * 4. 所有路径退出前关闭串口并释放动态内存。
 *
 * 返回值：
 * EXIT_SUCCESS 表示命令成功完成或用户正常退出；
 * EXIT_FAILURE 表示参数错误、串口打开失败、配置失败或读写失败。
 */
int main(int argc, char *argv[])
{
	const char *devname;                  /* 保存用户传入的串口设备节点，例如 /dev/ttySTM2。 */
	const char *cmd;                      /* 保存用户选择的命令，例如 recv、send、loop、term。 */
	struct tx_buffer tx = { 0 };          /* 保存 send/loop 模式解析后的发送数据，其他模式保持空。 */
	int baud;                             /* 保存解析后的整数波特率。 */
	int fd;                               /* 保存打开并配置好的串口文件描述符。 */
	int interval_ms = 0;                  /* 保存 loop 模式的周期发送间隔，其他模式不使用。 */
	int ret = EXIT_FAILURE;               /* 保存程序最终返回值，默认失败，成功路径显式改为 EXIT_SUCCESS。 */

	setvbuf(stdout, NULL, _IONBF, 0);     /* 关闭 stdout 缓冲，让串口调试输出实时显示。 */

	if (argc == 2 && strcmp(argv[1], "-h") == 0) { /* 用户只请求帮助。 */
		show_usage(argv[0]);             /* 打印帮助信息。 */
		return EXIT_SUCCESS;             /* 查看帮助属于成功操作。 */
	}

	if (argc < 4) {                       /* 基础格式至少需要 dev、baud、cmd 三个参数。 */
		show_usage(argv[0]);             /* 参数不足时打印帮助。 */
		return EXIT_FAILURE;             /* 参数错误返回失败。 */
	}

	devname = argv[1];                    /* 第一个参数是串口设备节点。 */
	if (parse_int_range(argv[2], 1, 4000000, &baud) < 0) { /* 第二个参数必须是合理范围内的波特率。 */
		fprintf(stderr, "invalid baud: %s\n", argv[2]); /* 打印波特率参数错误。 */
		return EXIT_FAILURE;             /* 返回失败。 */
	}
	cmd = argv[3];                        /* 第三个参数是命令模式。 */

	if (strcmp(cmd, "recv") == 0) {       /* recv 模式：只接收 F4 发来的数据。 */
		if (argc != 4) {                  /* recv 不需要额外参数。 */
			show_usage(argv[0]);          /* 参数数量不对时打印帮助。 */
			return EXIT_FAILURE;          /* 参数错误时不要打开串口，直接返回失败。 */
		}
	} else if (strcmp(cmd, "send") == 0) { /* send 模式：发送一次用户指定字符串。 */
		if (argc != 5) {                  /* send 必须额外带一个 text 参数。 */
			show_usage(argv[0]);          /* 参数数量不对时打印帮助。 */
			return EXIT_FAILURE;          /* 参数错误时不要打开串口，直接返回失败。 */
		}
		if (parse_tx_text(argv[4], &tx) < 0) /* 解析 \r\n 和 \xHH 等转义。 */
			return EXIT_FAILURE;          /* 解析失败时不要打开串口。 */
	} else if (strcmp(cmd, "loop") == 0) { /* loop 模式：周期发送用户指定字符串。 */
		if (argc != 6) {                  /* loop 必须带 text 和 interval_ms 两个额外参数。 */
			show_usage(argv[0]);          /* 参数数量不对时打印帮助。 */
			return EXIT_FAILURE;          /* 参数错误时不要打开串口，直接返回失败。 */
		} else if (parse_int_range(argv[5], MIN_LOOP_INTERVAL, MAX_LOOP_INTERVAL, &interval_ms) < 0) { /* 校验周期范围。 */
			fprintf(stderr, "invalid interval_ms: %s\n", argv[5]); /* 打印周期参数错误。 */
			return EXIT_FAILURE;          /* 周期非法时不要打开串口。 */
		}
		if (parse_tx_text(argv[4], &tx) < 0) /* 解析待周期发送的数据。 */
			return EXIT_FAILURE;          /* 解析失败时不要打开串口。 */
	} else if (strcmp(cmd, "term") == 0) { /* term 模式：键盘输入发送，串口输入显示。 */
		if (argc != 4) {                  /* term 不需要额外参数。 */
			show_usage(argv[0]);          /* 参数数量不对时打印帮助。 */
			return EXIT_FAILURE;          /* 参数错误时不要打开串口，直接返回失败。 */
		}
	} else {                              /* 未识别的命令。 */
		fprintf(stderr, "unknown command: %s\n", cmd); /* 打印未知命令。 */
		show_usage(argv[0]);              /* 打印帮助，方便用户改命令。 */
		return EXIT_FAILURE;              /* 未知命令时不要打开串口。 */
	}

	fd = open_serial(devname, baud);      /* 参数全部合法后再打开并配置串口。 */
	if (fd < 0) {                         /* 打开或配置失败时错误已经打印。 */
		free(tx.data);                    /* send/loop 可能已经申请了发送缓冲区，失败路径也要释放。 */
		return EXIT_FAILURE;             /* 返回失败。 */
	}

	if (strcmp(cmd, "recv") == 0)         /* recv 模式：只接收 F4 发来的数据。 */
		ret = receive_loop(fd);           /* 进入持续接收循环。 */
	else if (strcmp(cmd, "send") == 0)    /* send 模式：发送一次用户指定字符串。 */
		ret = send_once(fd, &tx);         /* 发送一次数据。 */
	else if (strcmp(cmd, "loop") == 0)    /* loop 模式：周期发送用户指定字符串。 */
		ret = loop_send(fd, &tx, interval_ms); /* 进入周期发送循环。 */
	else if (strcmp(cmd, "term") == 0)    /* term 模式：键盘输入发送，串口输入显示。 */
		ret = terminal_loop(fd);          /* 进入终端透传循环。 */

	free(tx.data);                        /* 释放 send/loop 申请的缓冲区；其他模式中 NULL 可安全传给 free。 */
	close(fd);                            /* 所有命令路径结束后关闭串口文件描述符。 */
	return ret;                           /* 返回命令执行结果。 */
}
