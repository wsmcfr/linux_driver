/***************************************************************
Copyright © ALIENTEK Co., Ltd. 1998-2029. All rights reserved.
文件名		: leddriver.c
作者	  	: 正点原子Linux团队
版本	   	: V1.0
描述	   	: 设备树下的platform驱动
其他	   	: 无
论坛 	   	: www.openedv.com
日志	   	: 初版V1.0 2019/8/13 正点原子Linux团队创建
***************************************************************/
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/ide.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/gpio.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/of_gpio.h>
#include <linux/semaphore.h>
#include <linux/timer.h>
#include <linux/irq.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/fs.h>
#include <linux/fcntl.h>
#include <linux/platform_device.h>
#include <asm/mach/map.h>
#include <asm/uaccess.h>
#include <asm/io.h>


#define LEDDEV_CNT		1				/* 设备号长度 	*/
#define LEDDEV_NAME		"dtsplatled"	/* 设备名字 	*/
#define LEDOFF 			0
#define LEDON 			1

/* leddev设备结构体 */
struct leddev_dev{
	dev_t devid;				/* 设备号	*/
	struct cdev cdev;			/* cdev		*/
	struct class *class;		/* 类 		*/
	struct device *device;		/* 设备		*/	
	struct device_node *node;	/* LED设备节点 */
	int gpio_led;				/* LED灯GPIO标号 */
};

struct leddev_dev leddev; 		/* led设备 */

/*
 * @description		: LED打开/关闭
 * @param - sta 	: LEDON(0) 打开LED，LEDOFF(1) 关闭LED
 * @return 			: 无
 */
void led_switch(u8 sta)
{
	if (sta == LEDON )
		gpio_set_value(leddev.gpio_led, 0);
	else if (sta == LEDOFF)
		gpio_set_value(leddev.gpio_led, 1);
}

static int led_gpio_init(struct device_node *nd)
{
	int ret;

	/* 从设备树中获取GPIO */
	leddev.gpio_led = of_get_named_gpio(nd, "led-gpio", 0);
	if(!gpio_is_valid(leddev.gpio_led)) {
        printk(KERN_ERR "leddev: Failed to get led-gpio\n");
        return -EINVAL;
    }
	
	/* 申请使用GPIO */
	ret = gpio_request(leddev.gpio_led, "LED0");
    if (ret) {
        printk(KERN_ERR "led: Failed to request led-gpio\n");
        return ret;
	}
	
	/* 将GPIO设置为输出模式并设置GPIO初始电平状态 */
    gpio_direction_output(leddev.gpio_led,1);
	
	return 0;
}

/*
 * @description		: 打开设备
 * @param - inode 	: 传递给驱动的inode
 * @param - filp 	: 设备文件，file结构体有个叫做private_data的成员变量
 * 					  一般在open的时候将private_data指向设备结构体。
 * @return 			: 0 成功;其他 失败
 */
static int led_open(struct inode *inode, struct file *filp)
{
	return 0;
}

/*
 * @description		: 向设备写数据 
 * @param - filp 	: 设备文件，表示打开的文件描述符
 * @param - buf 	: 要写给设备写入的数据
 * @param - cnt 	: 要写入的数据长度
 * @param - offt 	: 相对于文件首地址的偏移
 * @return 			: 写入的字节数，如果为负值，表示写入失败
 */
static ssize_t led_write(struct file *filp, const char __user *buf, size_t cnt, loff_t *offt)
{
	int retvalue;
	unsigned char databuf[1];
	unsigned char ledstat;

	retvalue = copy_from_user(databuf, buf, cnt);
	if(retvalue < 0) {
		printk("kernel write failed!\r\n");
		return -EFAULT;
	}
	
	ledstat = databuf[0];
	if (ledstat == LEDON) {
		led_switch(LEDON);
	} else if (ledstat == LEDOFF) {
		led_switch(LEDOFF);
	}
	return 0;
}

/* 设备操作函数 */
static struct file_operations led_fops = {
	.owner = THIS_MODULE,
	.open = led_open,
	.write = led_write,
};

/*
 * @description		: flatform驱动的probe函数，当驱动与
 * 					  设备匹配以后此函数就会执行
 * @param - dev 	: platform设备
 * @return 			: 0，成功;其他负值,失败
 */
static int led_probe(struct platform_device *pdev)
{	
	int ret;
	
	printk("led driver and device was matched!\r\n");
	
	/* 初始化 LED */
	ret = led_gpio_init(pdev->dev.of_node);
	if(ret < 0)
		return ret;
		
	/* 1、设置设备号 */
	ret = alloc_chrdev_region(&leddev.devid, 0, LEDDEV_CNT, LEDDEV_NAME);
	if(ret < 0) {
		pr_err("%s Couldn't alloc_chrdev_region, ret=%d\r\n", LEDDEV_NAME, ret);
		goto free_gpio;
	}
	
	/* 2、初始化cdev  */
	leddev.cdev.owner = THIS_MODULE;
	cdev_init(&leddev.cdev, &led_fops);
	
	/* 3、添加一个cdev */
	ret = cdev_add(&leddev.cdev, leddev.devid, LEDDEV_CNT);
	if(ret < 0)
		goto del_unregister;
	
	/* 4、创建类      */
	leddev.class = class_create(THIS_MODULE, LEDDEV_NAME);
	if (IS_ERR(leddev.class)) {
		goto del_cdev;
	}

	/* 5、创建设备 */
	leddev.device = device_create(leddev.class, NULL, leddev.devid, NULL, LEDDEV_NAME);
	if (IS_ERR(leddev.device)) {
		goto destroy_class;
	}
	
	return 0;
destroy_class:
	class_destroy(leddev.class);
del_cdev:
	cdev_del(&leddev.cdev);
del_unregister:
	unregister_chrdev_region(leddev.devid, LEDDEV_CNT);
free_gpio:
	gpio_free(leddev.gpio_led);
	return -EIO;
}

/*
 * @description		: platform驱动的remove函数，移除platform驱动的时候此函数会执行
 * @param - dev 	: platform设备
 * @return 			: 0，成功;其他负值,失败
 */
static int led_remove(struct platform_device *dev)
{
	gpio_set_value(leddev.gpio_led, 1); 	/* 卸载驱动的时候关闭LED */
	gpio_free(leddev.gpio_led);	/* 注销GPIO */
	cdev_del(&leddev.cdev);				/*  删除cdev */
	unregister_chrdev_region(leddev.devid, LEDDEV_CNT); /* 注销设备号 */
	device_destroy(leddev.class, leddev.devid);	/* 注销设备 */
	class_destroy(leddev.class); /* 注销类 */
	return 0;
}

/* 匹配列表 */
static const struct of_device_id led_of_match[] = {
	{ .compatible = "alientek,led" },
	{ /* Sentinel */ }
};

MODULE_DEVICE_TABLE(of, led_of_match);

/* platform驱动结构体 */
static struct platform_driver led_driver = {
	.driver		= {
		.name	= "stm32mp1-led",			/* 驱动名字，用于和设备匹配 */
		.of_match_table	= led_of_match, /* 设备树匹配表 		 */
	},
	.probe		= led_probe,
	.remove		= led_remove,
};
		
/*
 * @description	: 驱动模块加载函数
 * @param 		: 无
 * @return 		: 无
 */
static int __init leddriver_init(void)
{
	return platform_driver_register(&led_driver);
}

/*
 * @description	: 驱动模块卸载函数
 * @param 		: 无
 * @return 		: 无
 */
static void __exit leddriver_exit(void)
{
	platform_driver_unregister(&led_driver);
}

module_init(leddriver_init);
module_exit(leddriver_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("ALIENTEK");
MODULE_INFO(intree, "Y");