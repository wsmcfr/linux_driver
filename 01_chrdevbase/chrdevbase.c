#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/ide.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h>

#define CHRDEVBASE_MAJOR	 200				/* 主设备号 */
#define CHRDEVBASE_NAME		"chrdevbase" 	   /* 设备名     */

static char chrdevbase_readbuf[100] = {0};  // 读缓冲区
static char chrdevbase_writebuf[100] = {0}; // 写缓冲区



static char kerneldata[] = {"kernel data!"};  //应用从内核读取到数据

/*
 * @description		: 向设备写数据 
 * @param - filp 	: 设备文件，表示打开的文件描述符
 * @param - buf 	: 要写给设备写入的数据
 * @param - cnt 	: 要写入的数据长度
 * @param - offt 	: 相对于文件首地址的偏移
 * @return 			: 写入的字节数，如果为负值，表示写入失败
 */
static ssize_t chrdevbase_write(struct file *filp, const char __user *buf, size_t cnt, loff_t *offt)
{
    int ret = 0;
    // printk("chrdevbase_write\r\n");
    /* 将用户空间的数据复制到内核空间 */
    ret = copy_from_user(chrdevbase_writebuf, buf, cnt); // 将数据从用户空间复制到内核空间
    if (ret == 0) // copy_from_user成功
    {
        printk("kernel receive data from user success!\r\n");
        printk("data = %s\r\n", chrdevbase_writebuf); // 打印接收到的数据
        ret = cnt; // 返回实际写入的字节数
    }
    else // copy_from_user失败
    {
        printk("kernel receive data from user failed!\r\n");
        ret = -EFAULT; // 返回错误码
    }
    return 0;

}

/*
 * @description		: 从设备读取数据 
 * @param - filp 	: 要打开的设备文件(文件描述符)
 * @param - buf 	: 返回给用户空间的数据缓冲区
 * @param - cnt 	: 要读取的数据长度
 * @param - offt 	: 相对于文件首地址的偏移
 * @return 			: 读取的字节数，如果为负值，表示读取失败
 */static ssize_t chrdevbase_read(struct file *filp, char __user *buf, size_t cnt, loff_t *offt)
{
    int ret = 0;
    // printk("chrdevbase_read\r\n");
    memcpy(chrdevbase_readbuf, kerneldata, sizeof(kerneldata)); // 将内核数据复制到读缓冲区
     /* 将内核缓冲区的数据发送到用户空间 */
    ret = copy_to_user(buf, chrdevbase_readbuf, cnt); // 将数据从内核空间复制到用户空间
    if (ret == 0) // copy_to_user成功
    {
        printk("kernel send data to user success!\r\n");
        ret = cnt; // 返回实际读取的字节数
    }
    else // copy_to_user失败
    {
        printk("kernel send data to user failed!\r\n");
        ret = -EFAULT; // 返回错误码
    }
    return 0;
}

static int chrdevbase_open(struct inode *inode, struct file *filp)
{
    int ret = 0;
    // printk("chrdevbase_open\r\n");
    return ret;
}


static int chrdevbase_release(struct inode *inode, struct file *filp)
{
    int ret = 0;
    // printk("chrdevbase_release\r\n");
    return ret;
}




/*
 * 设备操作函数结构体
 */
static struct file_operations chrdevbase_fops = {
	.owner = THIS_MODULE,	
	.open = chrdevbase_open,
	.read = chrdevbase_read,
	.write = chrdevbase_write,
    .release = chrdevbase_release,

};

//入口函数
static int __init chardevbase_init(void)
{
    int ret = 0;
    printk("chardevbase_init\r\n");

    ret = register_chrdev(CHRDEVBASE_MAJOR, CHRDEVBASE_NAME, &chrdevbase_fops);   //注册字符设备驱动
    if(ret < 0)
    {
        printk("register_chrdev failed\r\n");
    }

	return ret;
}


//出口函数
static void __exit chardevbase_fini(void)
{
    unregister_chrdev(CHRDEVBASE_MAJOR, CHRDEVBASE_NAME); // 注销字符设备驱动
    printk("chardevbase_fini\r\n");
   
}




/*驱动的注册与卸载*/
module_init(chardevbase_init);  //入口函数
module_exit(chardevbase_fini);  //出口函数



MODULE_LICENSE("GPL");
MODULE_AUTHOR("CFR");
MODULE_INFO(intree, "Y");