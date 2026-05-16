#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "stdio.h"
#include "unistd.h"
#include "stdlib.h"
#include "string.h"

#define LEDON 1
#define LEDOFF 0
 
int main(int argc, char *argv[])
{
    int fd,retvalue;
    char *filername;
    unsigned char databuf[1];
    if(argc != 3)
    {
        printf("Usage:./app /dev/leddevbase \"data\"\r\n");
        return -1;
    }
    filername = argv[1];
    fd = open(filername, O_RDWR);

    if(fd < 0)
    {
        printf("open failed\r\n");
        return -1;
    }
    databuf[0] = atoi(argv[2]);

    retvalue = write(fd, databuf, sizeof(databuf));
    if(retvalue < 0)
    {
        printf("write %s failed\r\n", filername);
        close(fd);
        return -1;
    }



    retvalue = close(fd);
    if(retvalue < 0)
    {
        printf("close failed\r\n");
        return -1;
    }
    
    return 0;
}