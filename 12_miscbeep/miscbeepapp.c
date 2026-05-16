#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#define BEEP_ON  1
#define BEEP_OFF 0

void show_usage(char *argv)
{
    printf("Usage:\n");
    printf("    %s /dev/miscbeep 1    : Open Beep\n", argv);
    printf("    %s /dev/miscbeep 0    : Close Beep\n", argv);
}


int main(int argc, char *argv[])
{
    int fd;
    int ret = 0;
    char *filename;
    unsigned char databuf;
    if (argc < 3) {
        show_usage(argv[0]);
        return -1;
    }
    filename = argv[1];
    fd = open(filename,O_RDWR);
    if(fd < 0)
    {
        printf("file %s open failed!\r\n",filename);
        return -1;
    }
    databuf = atoi(argv[2]);
    ret = write(fd,&databuf,sizeof(databuf));
    if(ret < 0)
    {
        printf("BEEP Control failed!\r\n");
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}