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
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/miscdevice.h>
#include <asm/mach/map.h>
#include <asm/uaccess.h>

#include <asm/io.h>
#define MISCBEEP_MINOR   255
#define MISCBEEP_NAME  "miscbeep"
#define BEEP_ON  1
#define BEEP_OFF 0

/* 驱动设备结构体 */
struct miscbeep_dev {
    int beep_gpio;            //beep gpio
    struct device_node *nd;  //设备节点
    
};


static struct miscbeep_dev miscbeep;


void set_beep_value(struct miscbeep_dev *dev,unsigned char value)
{
    if(value == BEEP_ON)
    {
        gpio_set_value(dev->beep_gpio,0);
    }
    else
    {
        gpio_set_value(dev->beep_gpio,1);
    }
    
}

/* write: 接收应用层命令 */
static ssize_t miscbeep_write(struct file *filp, const char __user *buf,
                              size_t count, loff_t *ppos)
{
    int ret = 0;
    unsigned char beep_data = 0;
    struct miscbeep_dev *dev = filp->private_data;

    ret = copy_from_user(&beep_data,buf,count);
    if(ret < 0)
    {
        return -EINVAL;
    }
    set_beep_value(dev,beep_data);

    return count;
}

static int miscbeep_open(struct inode *inode, struct file *filp)
{
	filp->private_data = &miscbeep;
	return 0;
}

static int miscbeep_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static const struct file_operations miscbeep_fops = {
    .owner          = THIS_MODULE,
    .open           = miscbeep_open,
    .release        = miscbeep_release,
    .write          = miscbeep_write,
};


static struct miscdevice beep_miscdev = {
    .minor  = MISCBEEP_MINOR,
    .name   = MISCBEEP_NAME,
    .fops   = &miscbeep_fops
};


/* 设备树匹配 */
static const struct of_device_id miscbeep_of_match[] = {
    { .compatible = "atk,beep" },
    { }
};
MODULE_DEVICE_TABLE(of, miscbeep_of_match);

/* probe */
static int miscbeep_probe(struct platform_device *dev)
{
    int ret = 0;
    miscbeep.nd = dev ->dev.of_node;
    miscbeep.beep_gpio = of_get_named_gpio(miscbeep.nd,"beep-gpios",0);
    if(miscbeep.beep_gpio < 0)
    {
        ret = -EINVAL;
        goto fail_findgpio;
    }

    ret =   gpio_request(miscbeep.beep_gpio,"beep-gpios");
    if(ret)
    {
        printk("fail request %d gpio\r\n",miscbeep.beep_gpio);
        ret = -EINVAL;
        goto fail_findgpio;
    }
    ret = gpio_direction_output(miscbeep.beep_gpio,1);
    if(ret < 0)
    {
        goto fail_setoutput;
    }


    ret = misc_register(&beep_miscdev);
    if(ret < 0)
    {
        goto fail_setoutput;
    }
    return 0;


fail_setoutput:
    gpio_free(miscbeep.beep_gpio);
fail_findgpio:
    return ret;
}

/* remove */
static int miscbeep_remove(struct platform_device *dev)
{
    gpio_set_value(miscbeep.beep_gpio,1);
    gpio_free(miscbeep.beep_gpio);
    misc_deregister(&beep_miscdev);
    return 0;
}

static struct platform_driver miscbeep_driver = {
    .probe  = miscbeep_probe,
    .remove = miscbeep_remove,
    .driver = {
        .name = "atk-beep",
        .of_match_table = miscbeep_of_match,
    },
};

static int __init miscbeep_init(void)
{
    return platform_driver_register(&miscbeep_driver);
}

static void __exit miscbeep_exit(void)
{
    platform_driver_unregister(&miscbeep_driver);
}

module_init(miscbeep_init);
module_exit(miscbeep_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("CFR");
MODULE_DESCRIPTION("platform timer beep driver");






