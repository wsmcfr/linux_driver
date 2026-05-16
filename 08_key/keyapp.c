#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

int main(int argc, char *argv[])
{
    int fd;
    int ret;
    unsigned char key_value;

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
        ret = read(fd, &key_value, 1);
        if (ret < 0)
        {
            perror("read");
            close(fd);
            return -1;
        }

        printf("key = %d\n", key_value);
        usleep(200000);
    }

    close(fd);
    return 0;
}