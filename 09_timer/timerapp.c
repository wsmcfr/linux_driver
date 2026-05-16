#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

/* ioctl 魔数 */
#define TIMERLED_IOC_MAGIC   'L'

/* ioctl 命令 */
#define TIMERLED_CMD_ON      _IO(TIMERLED_IOC_MAGIC, 0)
#define TIMERLED_CMD_OFF     _IO(TIMERLED_IOC_MAGIC, 1)
#define TIMERLED_CMD_BLINK   _IOW(TIMERLED_IOC_MAGIC, 2, int)
#define TIMERLED_CMD_CUSTOM  _IOW(TIMERLED_IOC_MAGIC, 3, struct timerled_time)

/* 自定义亮灭时间结构体 */
struct timerled_time {
    int on_ms;
    int off_ms;
};

static void show_usage(char *name)
{
    printf("Usage:\n");
    printf("%s /dev/leddev on\n", name);
    printf("%s /dev/leddev off\n", name);
    printf("%s /dev/leddev blink 500\n", name);
    printf("%s /dev/leddev led 500 300\n", name);
}

int main(int argc, char *argv[])
{
    int fd;
    int period;
    int ret;
    struct timerled_time time;

    if (argc < 3) {
        show_usage(argv[0]);
        return -1;
    }

    fd = open(argv[1], O_RDWR);
    if (fd < 0) {
        perror("open");
        return -1;
    }

    if (strcmp(argv[2], "on") == 0) {
        ret = ioctl(fd, TIMERLED_CMD_ON);
        if (ret < 0) {
            perror("ioctl on");
            close(fd);
            return -1;
        }
    } 
    else if (strcmp(argv[2], "off") == 0) {
        ret = ioctl(fd, TIMERLED_CMD_OFF);
        if (ret < 0) {
            perror("ioctl off");
            close(fd);
            return -1;
        }
    } 
    else if (strcmp(argv[2], "blink") == 0) {
        if (argc != 4) {
            show_usage(argv[0]);
            close(fd);
            return -1;
        }

        period = atoi(argv[3]);
        if (period <= 0) {
            printf("invalid blink period\n");
            close(fd);
            return -1;
        }

        ret = ioctl(fd, TIMERLED_CMD_BLINK, &period);
        if (ret < 0) {
            perror("ioctl blink");
            close(fd);
            return -1;
        }
    } 
    else if (strcmp(argv[2], "led") == 0) {
        if (argc != 5) {
            show_usage(argv[0]);
            close(fd);
            return -1;
        }

        time.on_ms = atoi(argv[3]);
        time.off_ms = atoi(argv[4]);

        if (time.on_ms <= 0 || time.off_ms <= 0) {
            printf("invalid on/off time\n");
            close(fd);
            return -1;
        }

        ret = ioctl(fd, TIMERLED_CMD_CUSTOM, &time);
        if (ret < 0) {
            perror("ioctl custom");
            close(fd);
            return -1;
        }
    } 
    else {
        show_usage(argv[0]);
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}