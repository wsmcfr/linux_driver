#include <stdio.h>               /* 提供 printf、perror 等标准输入输出接口 */
#include <stdlib.h>              /* 提供 EXIT_SUCCESS、EXIT_FAILURE 等通用宏定义 */
#include <string.h>              /* 提供 strcmp 等字符串处理函数 */
#include <errno.h>               /* 提供 errno 错误码，便于打印系统调用失败原因 */
#include <fcntl.h>               /* 提供 open 函数和 O_RDONLY 等文件打开标志 */
#include <unistd.h>              /* 提供 read、close 等 POSIX 系统调用接口 */
#include <linux/input.h>         /* 提供 struct input_event、EV_KEY、KEY_1 等输入子系统定义 */

#define DEFAULT_INPUT_DEV "/dev/input/event0" /* 默认输入设备节点；用户不传参数时尝试打开这个节点 */

/*
 * show_usage - 打印应用程序使用说明
 * @progname: 当前程序名，通常来自 argv[0]
 *
 * 作用：
 * 1. 告诉用户程序应该如何启动。
 * 2. 说明如果不传参数，会默认监听哪个输入设备节点。
 * 3. 给出一个常见调用示例，方便直接照着执行。
 *
 * 返回值：
 * 无返回值，函数只负责打印说明信息。
 */
static void show_usage(const char *progname)
{
    printf("用法:\n");
    printf("    %s [input_event设备节点]\n", progname);
    printf("\n");
    printf("参数说明:\n");
    printf("    input_event设备节点   可选，例如 /dev/input/event0\n");
    printf("\n");
    printf("默认行为:\n");
    printf("    如果不传参数，程序默认打开 %s\n", DEFAULT_INPUT_DEV);
    printf("\n");
    printf("示例:\n");
    printf("    %s /dev/input/event1\n", progname);
}

/*
 * keycode_to_name - 把 Linux 按键码转换成更容易理解的字符串名称
 * @code: input 子系统上报的按键码，例如 KEY_1、KEY_2、KEY_3
 *
 * 作用：
 * 1. 把驱动里上报的数字按键码翻译成可读名字。
 * 2. 让打印日志时不只是看到一个数字，而是能直接看出是哪个按键。
 * 3. 如果将来驱动键值改了，也可以统一在这里扩展映射关系。
 *
 * 返回值：
 * - 返回一个只读字符串常量，表示当前按键的名字
 * - 如果传入的是未识别键值，则返回 "UNKNOWN_KEY"
 */
static const char *keycode_to_name(unsigned short code)
{
    switch (code) {
    case KEY_1:
        return "KEY_1";
    case KEY_2:
        return "KEY_2";
    case KEY_3:
        return "KEY_3";
    default:
        return "UNKNOWN_KEY";
    }
}

/*
 * keyvalue_to_action - 把按键事件值转换成动作说明字符串
 * @value: input 事件中的 value 字段
 *
 * 作用：
 * 1. 把 input_event.value 从数字翻译成“按下/松开/长按重复”等可读文本。
 * 2. 方便调试时快速判断当前事件到底代表什么语义。
 *
 * Linux 输入子系统里常见含义：
 * - 0: 按键松开
 * - 1: 按键按下
 * - 2: 长按保持时的重复事件（autorepeat）
 *
 * 返回值：
 * - 返回对应动作的字符串描述
 * - 如果值不在预期范围，则返回 "UNKNOWN"
 */
static const char *keyvalue_to_action(int value)
{
    switch (value) {
    case 0:
        return "RELEASE";
    case 1:
        return "PRESS";
    case 2:
        return "REPEAT";
    default:
        return "UNKNOWN";
    }
}

/*
 * print_key_event - 打印一次按键事件的详细信息
 * @event: 指向一条已经读取到的 input_event 事件
 *
 * 作用：
 * 1. 把驱动上报到用户空间的 EV_KEY 事件格式化打印出来。
 * 2. 只关注 KEY_1 / KEY_2 / KEY_3 三个目标按键，其他键值直接忽略。
 * 3. 输出事件时间、键值名、动作、原始 code/value，便于对照驱动调试。
 *
 * 返回值：
 * 无返回值，函数只负责格式化输出。
 */
static void print_key_event(const struct input_event *event)
{
    const char *key_name;   /* 保存按键名称字符串，例如 KEY_1 */
    const char *action;     /* 保存动作字符串，例如 PRESS / RELEASE */

    /*
     * 当前应用是专门给这个 3 键驱动配套的测试程序，
     * 所以这里只打印 KEY_1、KEY_2、KEY_3，其他按键事件统一忽略。
     */
    if (event->code != KEY_1 &&
        event->code != KEY_2 &&
        event->code != KEY_3) {
        return;
    }

    key_name = keycode_to_name(event->code);     /* 把按键码转换成可读字符串 */
    action = keyvalue_to_action(event->value);   /* 把事件值转换成动作描述 */

    /*
     * time 字段来自内核生成的事件时间戳：
     * - tv_sec  表示秒
     * - tv_usec 表示微秒
     * 打印它的意义，是方便你观察多个按键事件的先后顺序和抖动情况。
     */
    printf("[time=%ld.%06ld] key=%s action=%s code=%u value=%d\n",
           (long)event->time.tv_sec,
           (long)event->time.tv_usec,
           key_name,
           action,
           event->code,
           event->value);
}

/*
 * main - 应用程序主函数
 * @argc: 命令行参数个数
 * @argv: 命令行参数数组
 *
 * 主要流程：
 * 1. 解析用户是否传入 input 设备节点路径。
 * 2. 打开目标 /dev/input/eventX 设备。
 * 3. 进入死循环，不断读取 struct input_event 事件。
 * 4. 过滤 EV_KEY 类型事件，并打印 KEY_1/KEY_2/KEY_3 的动作信息。
 * 5. 如果读失败或设备断开，则打印错误并退出。
 *
 * 返回值：
 * - EXIT_SUCCESS: 正常结束
 * - EXIT_FAILURE: 打开设备或读取事件失败
 */
int main(int argc, char *argv[])
{
    const char *dev_path;                 /* 保存最终要打开的输入设备路径 */
    int fd;                               /* 保存 open 打开的设备文件描述符 */
    ssize_t ret;                          /* 保存每次 read 的返回值，用于判断是否读成功 */
    struct input_event event;             /* 保存从 /dev/input/eventX 读取到的一条输入事件 */

    /*
     * 如果用户带了 -h 或 --help，则只打印帮助信息，不继续执行。
     * 这样做的目的是让程序自说明，使用起来更方便。
     */
    if (argc >= 2 &&
        (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        show_usage(argv[0]);
        return EXIT_SUCCESS;
    }

    /*
     * 参数为空时使用默认节点；
     * 如果用户显式传了路径，则优先使用用户提供的路径。
     */
    if (argc >= 2) {
        dev_path = argv[1];
    } else {
        dev_path = DEFAULT_INPUT_DEV;
    }

    /*
     * 以只读方式打开 input 事件设备即可，因为应用只负责读事件，不需要写。
     * 如果这里打开失败，常见原因有：
     * 1. 设备节点路径写错
     * 2. 驱动没有成功注册 input 设备
     * 3. 当前用户没有访问 /dev/input/eventX 的权限
     */
    fd = open(dev_path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "打开输入设备失败: %s, errno=%d (%s)\n",
                dev_path, errno, strerror(errno));
        return EXIT_FAILURE;
    }

    printf("已打开输入设备: %s\n", dev_path);
    printf("开始监听 KEY_1 / KEY_2 / KEY_3 事件，按 Ctrl+C 结束程序。\n");

    /*
     * 死循环持续读取输入事件。
     * Linux input 子系统每次 read 通常会返回一个完整的 struct input_event。
     * 如果底层设备持续产生事件，这个循环就会一直打印。
     */
    while (1) {
        ret = read(fd, &event, sizeof(event));
        if (ret < 0) {
            /*
             * read 失败表示设备可能被拔掉、驱动异常，或者当前调用被信号中断。
             * 这里直接退出，便于你在调试时第一时间看到错误。
             */
            fprintf(stderr, "读取输入事件失败: errno=%d (%s)\n",
                    errno, strerror(errno));
            close(fd);
            return EXIT_FAILURE;
        }

        if (ret != (ssize_t)sizeof(event)) {
            /*
             * 正常情况下，input_event 设备一次应该读出完整结构体大小。
             * 如果长度不完整，通常说明读取状态异常，此时继续跑没有意义，直接退出更安全。
             */
            fprintf(stderr, "读取到的事件长度异常: ret=%ld, expect=%u\n",
                    (long)ret, (unsigned int)sizeof(event));
            close(fd);
            return EXIT_FAILURE;
        }

        /*
         * 只处理按键类事件。
         * 其他类型例如 EV_SYN 是同步事件，EV_MSC 是附加事件，
         * 对本测试程序来说不是重点，因此直接忽略。
         */
        if (event.type == EV_KEY) {
            print_key_event(&event);
        }
    }

    /*
     * 理论上死循环不会自然走到这里。
     * 保留 close 和 return，是为了让代码逻辑完整，也方便以后扩展退出条件。
     */
    close(fd);
    return EXIT_SUCCESS;
}
