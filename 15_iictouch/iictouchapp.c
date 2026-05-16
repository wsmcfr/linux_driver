#include <errno.h>               /* 引入 errno，系统调用失败时用它判断具体错误原因。 */
#include <fcntl.h>               /* 引入 open 函数和 O_RDONLY 打开标志。 */
#include <linux/input.h>         /* 引入 Linux input 事件结构和 EV_ABS、ABS_MT_* 等事件编码。 */
#include <stdbool.h>             /* 引入 bool、true、false 类型，便于表达 slot 是否按下。 */
#include <stdio.h>               /* 引入 printf、fprintf 等标准输出接口。 */
#include <stdlib.h>              /* 引入 EXIT_SUCCESS 和 EXIT_FAILURE 返回值宏。 */
#include <string.h>              /* 引入 strcmp 和 strerror 字符串处理接口。 */
#include <unistd.h>              /* 引入 read、close 等 POSIX 系统调用接口。 */

#define DEFAULT_EVENT_DEV	"/dev/input/event0" /* 默认监听的 input 设备节点；实际使用时可通过命令行覆盖。 */
#define MAX_TRACK_SLOTS		16                  /* 用户态最多记录 16 个 slot，足够覆盖 FT5426 的 5 点触摸。 */

/*
 * struct touch_slot - 用户态保存的一个触摸 slot 状态。
 * 这个结构体只用于测试程序打印坐标，不参与内核驱动逻辑。
 */
struct touch_slot {
	bool active;                                      /* 表示当前 slot 是否处于按下状态。 */
	int x;                                            /* 保存当前 slot 最近一次收到的 X 坐标。 */
	int y;                                            /* 保存当前 slot 最近一次收到的 Y 坐标。 */
};

/*
 * show_usage - 打印测试程序使用说明。
 * @progname: 当前程序名，通常来自 argv[0]。
 */
static void show_usage(const char *progname)
{
	printf("Usage: %s [event-dev]\n", progname);       /* 打印命令格式，说明可以传入 event 设备路径。 */
	printf("Example: %s /dev/input/event1\n", progname); /* 打印一个实际示例，方便直接照着运行。 */
	printf("Default: %s\n", DEFAULT_EVENT_DEV);        /* 打印默认监听设备，避免用户不传参数时不清楚监听对象。 */
}

/*
 * print_syn_report - 在一帧 input 事件结束时打印当前触点坐标。
 * @slots: 保存所有 slot 状态的数组。
 * @slot_count: slots 数组长度。
 */
static void print_syn_report(const struct touch_slot *slots, int slot_count)
{
	int i;                                             /* 循环变量，用于遍历所有 slot。 */
	bool any = false;                                  /* 标记当前帧是否存在至少一个有效触点。 */

	for (i = 0; i < slot_count; i++) {                 /* 遍历测试程序记录的所有 slot。 */
		if (!slots[i].active)                         /* 如果该 slot 当前没有按下，就不打印它。 */
			continue;                                  /* 跳过空闲 slot，继续检查下一个 slot。 */

		printf("slot %d: x=%d y=%d  ", i, slots[i].x, slots[i].y); /* 打印当前有效 slot 的坐标。 */
		any = true;                                   /* 标记已经打印过至少一个触点。 */
	}                                                   /* 结束 slot 遍历。 */

	if (any)                                            /* 如果当前帧存在触点，坐标输出后需要换行。 */
		printf("\n");                                  /* 打印换行，让每帧触摸状态显示在独立一行。 */
}

/*
 * handle_abs_event - 处理 EV_ABS 绝对坐标事件。
 * @slots: 保存所有 slot 状态的数组。
 * @current_slot: 当前正在更新的 slot 编号指针。
 * @event: 当前从 input 设备读到的事件。
 */
static void handle_abs_event(struct touch_slot *slots, int *current_slot,
			     const struct input_event *event)
{
	if (event->code == ABS_MT_SLOT) {                   /* ABS_MT_SLOT 表示内核切换当前正在描述的多点触摸 slot。 */
		if (event->value >= 0 && event->value < MAX_TRACK_SLOTS) /* 检查 slot 编号是否在测试程序数组范围内。 */
			*current_slot = event->value;              /* 更新当前 slot，后续坐标事件都归属于这个 slot。 */
		return;                                        /* slot 切换事件处理完成，直接返回。 */
	}                                                   /* 结束 ABS_MT_SLOT 处理。 */

	if (*current_slot < 0 || *current_slot >= MAX_TRACK_SLOTS) /* 如果当前 slot 编号非法，不能访问数组。 */
		return;                                        /* 直接返回，避免数组越界。 */

	switch (event->code) {                              /* 根据 ABS 事件 code 判断具体事件含义。 */
	case ABS_MT_TRACKING_ID:                            /* TRACKING_ID 表示一个触点的生命周期。 */
		slots[*current_slot].active = event->value >= 0; /* value >= 0 表示按下或跟踪中，value = -1 表示抬起。 */
		if (event->value >= 0)                          /* 如果 tracking id 非负，表示当前 slot 新触点按下。 */
			printf("slot %d down, tracking_id=%d\n", *current_slot, event->value); /* 打印 slot 按下和跟踪 ID。 */
		else                                            /* 如果 tracking id 为 -1，表示当前 slot 触点抬起。 */
			printf("slot %d up\n", *current_slot);      /* 打印 slot 抬起信息。 */
		break;                                          /* TRACKING_ID 事件处理完成，跳出 switch。 */
	case ABS_MT_POSITION_X:                              /* ABS_MT_POSITION_X 表示当前 slot 的 X 坐标。 */
		slots[*current_slot].x = event->value;          /* 保存当前 slot 的最新 X 坐标。 */
		break;                                          /* X 坐标事件处理完成，跳出 switch。 */
	case ABS_MT_POSITION_Y:                              /* ABS_MT_POSITION_Y 表示当前 slot 的 Y 坐标。 */
		slots[*current_slot].y = event->value;          /* 保存当前 slot 的最新 Y 坐标。 */
		break;                                          /* Y 坐标事件处理完成，跳出 switch。 */
	default:                                             /* 其他 ABS 事件当前测试程序不需要处理。 */
		break;                                          /* 对未知 ABS 事件保持忽略。 */
	}                                                    /* 结束 ABS 事件分类处理。 */
}

/*
 * main - FT5426 触摸测试程序入口。
 * @argc: 命令行参数数量。
 * @argv: 命令行参数数组；argv[1] 可指定 /dev/input/eventX。
 */
int main(int argc, char *argv[])
{
	const char *devname = DEFAULT_EVENT_DEV;             /* 默认打开 event0；如果用户传参则改为用户指定路径。 */
	struct touch_slot slots[MAX_TRACK_SLOTS] = { 0 };    /* 初始化 slot 状态数组，所有 slot 默认未按下且坐标为 0。 */
	struct input_event event;                            /* 保存每次 read 读到的一条 input_event。 */
	int current_slot = 0;                                /* 当前正在更新的 slot 编号，input 多点协议默认从 slot 0 开始。 */
	int fd;                                              /* 保存打开的 /dev/input/eventX 文件描述符。 */

	if (argc > 2 || (argc == 2 && !strcmp(argv[1], "-h"))) { /* 参数过多或者用户传入 -h 时打印帮助。 */
		show_usage(argv[0]);                            /* 调用帮助函数输出使用说明。 */
		return argc > 2 ? EXIT_FAILURE : EXIT_SUCCESS;  /* 参数错误返回失败，主动看帮助返回成功。 */
	}                                                    /* 结束参数合法性检查。 */

	if (argc == 2)                                       /* 如果用户传入一个参数，就把它当作 event 设备路径。 */
		devname = argv[1];                              /* 使用用户指定的 /dev/input/eventX 路径。 */

	fd = open(devname, O_RDONLY);                        /* 以只读方式打开 input event 设备。 */
	if (fd < 0) {                                        /* 如果打开失败，通常是设备不存在或权限不足。 */
		fprintf(stderr, "open %s failed: %s\n", devname, strerror(errno)); /* 打印打开失败原因。 */
		return EXIT_FAILURE;                            /* 返回失败，程序结束。 */
	}                                                    /* 结束设备打开失败处理。 */

	printf("Listening on %s, press Ctrl+C to stop.\n", devname); /* 打印当前正在监听的 event 设备。 */

	while (1) {                                          /* 持续读取 input 事件，直到用户 Ctrl+C 终止程序。 */
		ssize_t len = read(fd, &event, sizeof(event));   /* 阻塞读取一条 input_event；没有触摸事件时会等待。 */

		if (len < 0) {                                  /* 如果 read 返回负数，说明读取失败或被信号中断。 */
			if (errno == EINTR)                         /* EINTR 表示 read 被信号打断，不是严重错误。 */
				continue;                               /* 继续下一轮读取。 */
			fprintf(stderr, "read %s failed: %s\n", devname, strerror(errno)); /* 打印 read 失败原因。 */
			close(fd);                                  /* 关闭已经打开的 event 设备。 */
			return EXIT_FAILURE;                        /* 返回失败，程序结束。 */
		}                                                /* 结束 read 失败处理。 */

		if (len != sizeof(event))                        /* 如果读到的数据长度不等于一个 input_event，说明数据不完整。 */
			continue;                                    /* 忽略不完整数据，继续读取下一条事件。 */

		if (event.type == EV_ABS) {                      /* EV_ABS 表示绝对坐标事件，触摸坐标属于这一类。 */
			handle_abs_event(slots, &current_slot, &event); /* 交给 ABS 事件处理函数解析 slot 和坐标。 */
		} else if (event.type == EV_KEY && event.code == BTN_TOUCH) { /* BTN_TOUCH 表示整体触摸按下或抬起状态。 */
			printf("BTN_TOUCH %s\n", event.value ? "down" : "up"); /* 打印整体触摸状态。 */
		} else if (event.type == EV_SYN && event.code == SYN_REPORT) { /* SYN_REPORT 表示一帧 input 事件结束。 */
			print_syn_report(slots, MAX_TRACK_SLOTS);   /* 在一帧结束时打印当前所有有效触点坐标。 */
		}                                                /* 其他事件类型当前测试程序不处理。 */
	}                                                    /* 结束事件读取循环。 */

	close(fd);                                           /* 理论上 while 不会自然退出，这里保留关闭逻辑以保持代码完整。 */
	return EXIT_SUCCESS;                                 /* 返回成功，表示程序正常结束。 */
}
