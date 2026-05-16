#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/ide.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h>

#define LEDDEV_MAJOR	 200				/* 主设备号 */
#define LEDDEV_NAME		"leddev" 	       /* 设备名     */
#define LEDOFF 0
#define LEDON 1



// static char leddev_readbuf[100] = {0};  // 读缓冲区
// static char leddev_writebuf[100] = {0}; // 写缓冲区

/* 寄存器物理地址 */
#define PERIPH_BASE     		     	(0x40000000)
#define MPU_AHB4_PERIPH_BASE			(PERIPH_BASE + 0x10000000)
#define RCC_BASE        		    	(MPU_AHB4_PERIPH_BASE + 0x0000)	
#define RCC_MP_AHB4ENSETR				(RCC_BASE + 0XA28)
#define GPIOI_BASE						(MPU_AHB4_PERIPH_BASE + 0xA000)	
#define GPIOI_MODER      			    (GPIOI_BASE + 0x0000)	
#define GPIOI_OTYPER      			    (GPIOI_BASE + 0x0004)	
#define GPIOI_OSPEEDR      			    (GPIOI_BASE + 0x0008)	
#define GPIOI_PUPDR      			    (GPIOI_BASE + 0x000C)	
#define GPIOI_BSRR      			    (GPIOI_BASE + 0x0018)


/* 映射后的寄存器虚拟地址指针 */
static void __iomem *MPU_AHB4_PERIPH_RCC_PI;
static void __iomem *GPIOI_MODER_PI;
static void __iomem *GPIOI_OTYPER_PI;
static void __iomem *GPIOI_OSPEEDR_PI;
static void __iomem *GPIOI_PUPDR_PI;
static void __iomem *GPIOI_BSRR_PI;




//地址映射函数
static int leddev_ioremap(void)
{
    int ret = 0;
    /* 将物理地址映射到内核虚拟地址空间 */
    MPU_AHB4_PERIPH_RCC_PI = ioremap(RCC_MP_AHB4ENSETR, 4); // 映射RCC寄存器
    if (MPU_AHB4_PERIPH_RCC_PI == NULL) // 映射失败
    {
        printk("ioremap RCC failed!\r\n");
        ret = -ENOMEM; // 返回错误码
        goto fail_ioremap_rcc; // 跳转到错误处理
    }
    GPIOI_MODER_PI = ioremap(GPIOI_MODER, 4); // 映射GPIOI_MODER寄存器
    if (GPIOI_MODER_PI == NULL) // 映射失败
    {
        printk("ioremap GPIOI_MODER failed!\r\n");
        ret = -ENOMEM; // 返回错误码
        goto fail_ioremap_gpioi_moder; // 跳转到错误处理
    }
    GPIOI_OTYPER_PI = ioremap(GPIOI_OTYPER, 4); // 映射GPIOI_OTYPER寄存器
    if (GPIOI_OTYPER_PI == NULL) // 映射失败
    {
        printk("ioremap GPIOI_OTYPER failed!\r\n");
        ret = -ENOMEM; // 返回错误码
        goto fail_ioremap_gpioi_otyper; // 跳转到错误处理
    }
    GPIOI_OSPEEDR_PI = ioremap(GPIOI_OSPEEDR, 4); // 映射GPIOI_OSPEEDR寄存器
    if (GPIOI_OSPEEDR_PI == NULL) // 映射失败
    {
        printk("ioremap GPIOI_OSPEEDR failed!\r\n");
        ret = -ENOMEM; // 返回错误码
        goto fail_ioremap_gpioi_ospeedr; // 跳转到错误处理
    }
    GPIOI_PUPDR_PI = ioremap(GPIOI_PUPDR, 4); // 映射GPIOI_PUPDR寄存器
    if (GPIOI_PUPDR_PI == NULL) // 映射失败
    {
        printk("ioremap GPIOI_PUPDR failed!\r\n");
        ret = -ENOMEM; // 返回错误码
        goto fail_ioremap_gpioi_pupdr; // 跳转到错误处理
    }
    GPIOI_BSRR_PI = ioremap(GPIOI_BSRR, 4); // 映射GPIOI_BSRR寄存器
    if (GPIOI_BSRR_PI == NULL) // 映射失败
    {
        printk("ioremap GPIOI_BSRR failed!\r\n");
        ret = -ENOMEM; // 返回错误码
        goto fail_ioremap_gpioi_bsrr; // 跳转到错误处理
    }
    return 0; // 映射成功，返回0
fail_ioremap_gpioi_bsrr:
    iounmap(GPIOI_PUPDR_PI); // 取消映射GPIOI_PUPDR寄存器
fail_ioremap_gpioi_pupdr:
    iounmap(GPIOI_OSPEEDR_PI); // 取消映射GPIOI_OSPEEDR寄存器
fail_ioremap_gpioi_ospeedr:
    iounmap(GPIOI_OTYPER_PI); // 取消映射GPIOI_OTYPER寄存器 
fail_ioremap_gpioi_otyper:
    iounmap(GPIOI_MODER_PI); // 取消映射GPIOI_MODER
fail_ioremap_gpioi_moder:
    iounmap(MPU_AHB4_PERIPH_RCC_PI); // 取消映射RCC寄存器
fail_ioremap_rcc:
    return ret; // 返回错误码                   
}
//取消映射函数
static void leddev_iounmap(void)
{
    iounmap(GPIOI_BSRR_PI); // 取消映射GPIOI_BSRR寄存器
    iounmap(GPIOI_PUPDR_PI); // 取消映射GPIOI_PUPDR寄存器
    iounmap(GPIOI_OSPEEDR_PI); // 取消映射GPIOI_OSPEEDR寄存器
    iounmap(GPIOI_OTYPER_PI); // 取消映射GPIOI_OTYPER寄存器 
    iounmap(GPIOI_MODER_PI); // 取消映射GPIOI_MODER
    iounmap(MPU_AHB4_PERIPH_RCC_PI); // 取消映射RCC寄存器
}


// static char kerneldata[] = {"kernel data!"};  //应用从内核读取到数据

void led_switch(u8 sta)
{
    u32 val = 0;
    if(sta == LEDON) // 如果状态为LEDON
    {
        readl(GPIOI_BSRR_PI); 
        val &= ~(0x1 << 16); 
        val |= (0x1 << 16); 
        writel(val, GPIOI_BSRR_PI); 
    }
    else if(sta == LEDOFF) // 如果状态为LEDOFF
    {
        readl(GPIOI_BSRR_PI); // 读取GPIOI_BSRR寄存器的值
        val &= ~(0x1 << 0); // 清除GPIOI0的输出 
        val |= (0x1 << 0); // 设置GPIOI0的输出为1，关闭LED
        writel(val, GPIOI_BSRR_PI); // 将修改后的值写回GPIOI_BSRR寄存器


    }
}

/*
 * @description		: 向设备写数据 
 * @param - filp 	: 设备文件，表示打开的文件描述符
 * @param - buf 	: 要写给设备写入的数据
 * @param - cnt 	: 要写入的数据长度
 * @param - offt 	: 相对于文件首地址的偏移
 * @return 			: 写入的字节数，如果为负值，表示写入失败
 */
static ssize_t leddev_write(struct file *filp, const char __user *buf, size_t cnt, loff_t *offt)
{
    int ret = 0;
    // printk("leddev_write\r\n");
    /* 将用户空间的数据复制到内核空间 */
    unsigned char databuf[1];
    unsigned char ledstate;
    ret = copy_from_user(databuf, buf, cnt); // 将数据从用户空间复制到内核空间
    if (ret == 0) // copy_from_user成功
    {
        printk("kernel receive data from user success!\r\n");
        // printk("data = %u\r\n", databuf); // 打印接收到的数据
        ret = cnt; // 返回实际写入的字节数
    }
    else // copy_from_user失败
    {
        printk("kernel receive data from user failed!\r\n");
        ret = -EFAULT; // 返回错误码
    }
    ledstate = databuf[0]; // 获取LED状态
    led_switch(ledstate); // 切换LED状态
    return 0;

}

/*
 * @description		: 从设备读取数据 
 * @param - filp 	: 要打开的设备文件(文件描述符)
 * @param - buf 	: 返回给用户空间的数据缓冲区
 * @param - cnt 	: 要读取的数据长度
 * @param - offt 	: 相对于文件首地址的偏移
 * @return 			: 读取的字节数，如果为负值，表示读取失败
 */
// static ssize_t leddev_read(struct file *filp, char __user *buf, size_t cnt, loff_t *offt)
// {
//     int ret = 0;
//     // printk("leddev_read\r\n");
//     memcpy(leddev_readbuf, kerneldata, sizeof(kerneldata)); // 将内核数据复制到读缓冲区
//      /* 将内核缓冲区的数据发送到用户空间 */
//     ret = copy_to_user(buf, leddev_readbuf, cnt); // 将数据从内核空间复制到用户空间
//     if (ret == 0) // copy_to_user成功
//     {
//         printk("kernel send data to user success!\r\n");
//         ret = cnt; // 返回实际读取的字节数
//     }
//     else // copy_to_user失败
//     {
//         printk("kernel send data to user failed!\r\n");
//         ret = -EFAULT; // 返回错误码
//     }
//     return 0;
// }

static int leddev_open(struct inode *inode, struct file *filp)
{
    int ret = 0;
    // printk("leddev_open\r\n");
    return ret;
}


static int leddev_release(struct inode *inode, struct file *filp)
{
    int ret = 0;
    // printk("leddev_release\r\n");
    return ret;
}




/*
 * 设备操作函数结构体
 */
static struct file_operations leddev_fops = {
	.owner = THIS_MODULE,	
	.open = leddev_open,
	.write = leddev_write,
    .release = leddev_release,

};
//入口函数
static int __init leddev_init(void)
{
    int ret = 0;
    u32 val = 0;
    printk("led_init\r\n");
    /*1. 寄存器地址映射*/
    leddev_ioremap(); // 地址映射

    /*2. 使能GPIOI时钟*/
    val = readl(MPU_AHB4_PERIPH_RCC_PI); // 读取RCC寄存器的值
    val &= ~(1 << 8); // 清除GPIOI时钟使能位
    val |= (1 << 8); // 设置GPIOI时钟使能位
    writel(val, MPU_AHB4_PERIPH_RCC_PI); // 将修改后的值写回RCC寄存器

    /*3. GPIOI0设置为输出引脚*/
    val = readl(GPIOI_MODER_PI); // 读取GPIOI_MODER寄存器的值
    val &= ~(0x3 << 0); // 清除GPIOI0的模式
    val |= (0x1 << (2 * 0)); // 设置GPIOI0为通用输出模式
    writel(val, GPIOI_MODER_PI); // 将修改后的值写回GPIOI_MODER寄存器

    /*4. GPIOI0设置为推挽输出*/
    val = readl(GPIOI_OTYPER_PI); // 读取GPIOI_OTYPER寄存器的值
    val &= ~(0x1 << 0); // 清除GPIOI0的输出
    writel(val, GPIOI_OTYPER_PI); // 将修改后的值写回GPIOI_OTYPER寄存器

    /*5. GPIOI0设置为超高速*/
    val = readl(GPIOI_OSPEEDR_PI); // 读取GPIOI_OSPEEDR寄存器的值
    val &= ~(0x3 << (2 * 0)); // 清除GPIOI0的输出速度
    val |= (0x3 << (2 * 0)); // 设置GPIOI0为超高速
    writel(val, GPIOI_OSPEEDR_PI); // 将修改后的值写回GPIOI_OSPEEDR寄存器

    /*6. GPIOI0设置为上拉电阻*/
	val = readl(GPIOI_PUPDR_PI);
	val &= ~(0X3 << 0); /* bit0:1 清零*/
	val |= (0x1 << 0); /*bit0:1 设置为01*/
	writel(val,GPIOI_PUPDR_PI);

	/* 7、默认关闭LED */
	val = readl(GPIOI_BSRR_PI);
    val &= ~(0x1 << 0); // 清除GPIOI0的输出
	val |= (0x1 << 0);	
	writel(val, GPIOI_BSRR_PI);

     /*4. 注册字符设备驱动 */
    ret = register_chrdev(LEDDEV_MAJOR, LEDDEV_NAME, &leddev_fops);   //注册字符设备驱动
    if(ret < 0)
    {
        printk("register_leddev failed\r\n");
        goto fail_register_chrdev; // 注册失败，跳转到错误处理
    }
 
	return 0;

fail_register_chrdev:
    return ret; // 返回错误码
}


//出口函数
static void __exit leddev_fini(void)
{
    leddev_iounmap(); // 取消映射
    unregister_chrdev(LEDDEV_MAJOR, LEDDEV_NAME); // 注销字符设备驱动
    printk("leddev_fini\r\n");
   
}




/*驱动的注册与卸载*/
module_init(leddev_init);  //入口函数
module_exit(leddev_fini);  //出口函数



MODULE_LICENSE("GPL");
MODULE_AUTHOR("CFR");
MODULE_INFO(intree, "Y");