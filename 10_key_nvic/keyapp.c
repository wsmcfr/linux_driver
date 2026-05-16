#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[])
{
    int fd;
    int ret;
    char buf[32];

    if (argc != 2)
    {
        printf("Usage: %s /dev/gpiokey_platform\n", argv[0]);
        return -1;
    }

    fd = open(argv[1], O_RDWR);
    if (fd < 0)
    {
        perror("open");
        return -1;
    }

    while (1)
    {
        memset(buf, 0, sizeof(buf));
        ret = read(fd, buf, sizeof(buf));
        if (ret < 0)
        {
            perror("read");
            close(fd);
            return -1;
        }

        if (ret > 0)
        {
            printf("event: %s\n", buf);
        }

    }

    close(fd);
    return 0;
}