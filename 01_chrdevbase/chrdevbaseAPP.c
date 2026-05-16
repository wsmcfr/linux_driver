#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "stdio.h"
#include "unistd.h"
#include "stdlib.h"
#include "string.h"


static char userdata[] = {"user data!"};  //应用写入内核的数据
 
int main(int argc, char *argv[])
{
    int fd,retvalue;
    char *filername;

    printf("hello linux app\r\n");
    if(argc != 3)
    {
        printf("Usage:./app /dev/chrdevbase \"data\"\r\n");
        return -1;
    }
    filername = argv[1];
    fd = open(filername, O_RDWR);

    if(fd < 0)
    {
        printf("open failed\r\n");
        return -1;
    }
    
    if(atoi(argv[2]) == 2)
    {
        char writebuf[100] = {0};
        memcpy(writebuf, userdata, sizeof(userdata));
        retvalue = write(fd, writebuf, sizeof(writebuf));
        if(retvalue < 0)
        {
            printf("write %s failed\r\n", filername);
            return -1;
        }
        printf("write data = %s\r\n", writebuf);
    }
    else if(atoi(argv[2]) == 1)
    {
        char readbuf[100] = {0};
        retvalue = read(fd, readbuf, sizeof(readbuf));
        if(retvalue < 0)
        {
            printf("read %s failed\r\n", filername);
            return -1;
        }
        printf("read data = %s\r\n", readbuf);
    }

    retvalue = close(fd);
    if(retvalue < 0)
    {
        printf("close failed\r\n");
        return -1;
    }
    
    return 0;
}

