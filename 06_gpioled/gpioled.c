#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/cdev.h>
#include <linux/gpio.h>
#include <linux/string.h>

#define GPIOLED_CNT 1
#define GPIOLED_NAME "gpioled"
#define LED_ON  1
#define LED_OFF 0

/*
 * gpioled 设备结构体
 * 该结构体用于保存字符设备、设备树节点以及 GPIO 相关资源。
 */
struct gpioleddev
{
	dev_t devid;                /* 设备号 */
	int major;                  /* 主设备号 */
	int minor;                  /* 次设备号 */
	struct cdev cdev;           /* 字符设备 */
	struct class *class;        /* 设备类 */
	struct device *device;      /* 设备对象 */
	struct device_node *nd;     /* 设备树节点 */
	int led_gpio;               /* LED 对应的 GPIO 编号 */
};

/* 定义一个全局的 gpioled 设备对象，整个驱动只管理这一个 LED 设备 */
static struct gpioleddev gpioled;


/*
 * 打开设备
 * 将设备结构体地址保存到 file 的 private_data 中，
 * 这样后续 write/release 等操作就可以通过 filp 获取设备私有数据。
 */
static int gpioled_open(struct inode *inode, struct file *filp)
{
	filp->private_data = &gpioled;
	return 0;
}

/*
 * 关闭设备
 * 当前驱动没有额外资源需要在 release 中释放，因此直接返回 0。
 */
static int gpioled_release(struct inode *inode, struct file *filp)
{
	return 0;
}

/*
 * 向设备写数据
 * 用户空间向设备节点写入 1 个字节：
 * 1 表示点亮 LED，0 表示熄灭 LED。
 */
static ssize_t gpioled_write(struct file *filp, const char __user *buf, size_t count, loff_t *ppos)
{
	int ret;
	unsigned char databuf[1];
	unsigned char ledstat;
	struct gpioleddev *dev = filp->private_data;

	if (count < 1)
	{
		return -EINVAL;
	}

	ret = copy_from_user(databuf, buf, 1);
	if (ret)
	{
		return -EFAULT;
	}

	ledstat = databuf[0];

	if (ledstat == LED_ON)
	{
		gpio_set_value(dev->led_gpio, 0); /* 低电平点亮 LED */
	}
	else if (ledstat == LED_OFF)
	{
		gpio_set_value(dev->led_gpio, 1); /* 高电平熄灭 LED */
	}
	else
	{
		return -EINVAL;
	}

	return count;
}

/*
 * 设备操作函数结构体
 */
static struct file_operations gpioled_fops = {
	.owner   = THIS_MODULE,
	.open    = gpioled_open,
	.write   = gpioled_write,
	.release = gpioled_release,
};


/*
 * 驱动入口函数
 * 完成字符设备注册、设备节点创建、设备树节点查找、GPIO 申请以及 GPIO 初始化。
 */
static int __init led_init(void)
{
	int ret = 0;
	const char *str;

	/*
	 * 先将关键成员初始化为安全值，便于后续在错误路径中统一释放资源。
	 * major/minor 使用静态全局变量的默认初值 0，即默认动态申请设备号。
	 */
	gpioled.devid = 0;
	gpioled.class = NULL;
	gpioled.device = NULL;
	gpioled.nd = NULL;
	gpioled.led_gpio = -1;

	/*
	 * 1. 创建设备号。
	 * 如果主设备号不为 0，则使用指定主设备号；
	 * 如果主设备号为 0，则动态申请主设备号。
	 */
	if (gpioled.major)
	{
		gpioled.devid = MKDEV(gpioled.major, gpioled.minor);
		ret = register_chrdev_region(gpioled.devid, GPIOLED_CNT, GPIOLED_NAME);
		if (ret < 0)
		{
			printk("gpioled register fail\r\n");
			ret = -EINVAL;
			goto register_fail;
		}
	}
	else
	{
		ret = alloc_chrdev_region(&gpioled.devid, 0, GPIOLED_CNT, GPIOLED_NAME);
		if (ret < 0)
		{
			printk("gpioled register fail\r\n");
			ret = -EINVAL;
			goto register_fail;
		}
		gpioled.major = MAJOR(gpioled.devid);
		gpioled.minor = MINOR(gpioled.devid);
	}
	printk("gpioled major=%d, minor=%d\r\n", gpioled.major, gpioled.minor);

	/* 2. 初始化字符设备并添加到内核字符设备框架中 */
	cdev_init(&gpioled.cdev, &gpioled_fops);
	gpioled.cdev.owner = THIS_MODULE;
	ret = cdev_add(&gpioled.cdev, gpioled.devid, GPIOLED_CNT);
	if (ret < 0)
	{
		printk("gpioled cdev add failed\r\n");
		ret = -EINVAL;
		goto cdev_fail;
	}

	/* 3. 创建设备类 */
	gpioled.class = class_create(THIS_MODULE, GPIOLED_NAME);
	if (IS_ERR(gpioled.class))
	{
		printk("gpioled create class failed\r\n");
		ret = PTR_ERR(gpioled.class);
		gpioled.class = NULL;
		goto class_fail;
	}

	/* 4. 创建设备节点，创建设备后用户空间才会出现对应的 /dev/gpioled */
	gpioled.device = device_create(gpioled.class, NULL, gpioled.devid, NULL, GPIOLED_NAME);
	if (IS_ERR(gpioled.device))
	{
		printk("gpioled create device failed\r\n");
		ret = PTR_ERR(gpioled.device);
		gpioled.device = NULL;
		goto device_fail;
	}

	/* 5. 查找设备树中的 gpioled 节点 */
	gpioled.nd = of_find_node_by_path("/gpioled");
	if (gpioled.nd == NULL)
	{
		printk("gpioled node can not found\r\n");
		ret = -EINVAL;
		goto nd_fail;
	}

	/* 6. 读取 status 属性，并确认节点状态为 okay */
	ret = of_property_read_string(gpioled.nd, "status", &str);
	if (ret < 0)
	{
		printk("gpioled read status failed\r\n");
		ret = -EINVAL;
		goto status_fail;
	}
	if (strcmp(str, "okay") != 0)
	{
		printk("gpioled status is not okay\r\n");
		ret = -EINVAL;
		goto status_fail;
	}

	/* 7. 读取 compatible 属性，并确认兼容字符串匹配 */
	ret = of_property_read_string(gpioled.nd, "compatible", &str);
	if (ret < 0)
	{
		printk("gpioled failed to get compatible property\r\n");
		ret = -EINVAL;
		goto status_fail;
	}
	if (strcmp(str, "atk,gpioled") != 0)
	{
		printk("gpioled compatible match failed\r\n");
		ret = -EINVAL;
		goto status_fail;
	}

	/* 8. 获取 led-gpio 属性，得到 LED 所使用的 GPIO 编号 */
	gpioled.led_gpio = of_get_named_gpio(gpioled.nd, "led-gpio", 0);
	if (gpioled.led_gpio < 0)
	{
		printk("gpioled can not get led-gpio\r\n");
		ret = -EINVAL;
		goto status_fail;
	}
	printk("gpioled led-gpio num = %d\r\n", gpioled.led_gpio);

	/* 9. 向 GPIO 子系统申请该 GPIO，避免被其他驱动重复使用 */
	ret = gpio_request(gpioled.led_gpio, "LED-GPIO");
	if (ret < 0)
	{
		printk("gpioled request led-gpio failed\r\n");
		goto gpio_request_fail;
	}

	/*
	 * 10. 将 GPIO 配置为输出模式，并默认输出高电平。
	 * 由于该 LED 为低电平点亮，因此这里输出高电平表示默认熄灭。
	 */
	ret = gpio_direction_output(gpioled.led_gpio, 1);
	if (ret < 0)
	{
		printk("gpioled set gpio direction failed\r\n");
		goto gpio_dir_fail;
	}

	return 0;
gpio_dir_fail:
	if (gpio_is_valid(gpioled.led_gpio))
	{
		gpio_free(gpioled.led_gpio);
		gpioled.led_gpio = -1;
	}

gpio_request_fail:
status_fail:
	if (gpioled.nd)
	{
		of_node_put(gpioled.nd);
		gpioled.nd = NULL;
	}

nd_fail:
	if (!IS_ERR_OR_NULL(gpioled.device))
	{
		device_destroy(gpioled.class, gpioled.devid);
		gpioled.device = NULL;
	}

device_fail:
	if (!IS_ERR_OR_NULL(gpioled.class))
	{
		class_destroy(gpioled.class);
		gpioled.class = NULL;
	}

class_fail:
	cdev_del(&gpioled.cdev);

cdev_fail:
	unregister_chrdev_region(gpioled.devid, GPIOLED_CNT);

register_fail:
	return ret;
}

/*
 * 驱动出口函数
 * 卸载驱动时按与初始化相反的顺序释放资源。
 */
static void __exit led_exit(void)
{
	if (gpio_is_valid(gpioled.led_gpio))
	{
		gpio_set_value(gpioled.led_gpio, 1); /* 卸载前让 LED 保持熄灭状态 */
		gpio_free(gpioled.led_gpio);         /* 释放 GPIO */
		gpioled.led_gpio = -1;
	}

	if (gpioled.nd)
	{
		of_node_put(gpioled.nd);
		gpioled.nd = NULL;
	}

	if (!IS_ERR_OR_NULL(gpioled.device) && !IS_ERR_OR_NULL(gpioled.class))
	{
		device_destroy(gpioled.class, gpioled.devid);
		gpioled.device = NULL;
	}

	if (!IS_ERR_OR_NULL(gpioled.class))
	{
		class_destroy(gpioled.class);
		gpioled.class = NULL;
	}

	cdev_del(&gpioled.cdev);
	unregister_chrdev_region(gpioled.devid, GPIOLED_CNT);
}

module_init(led_init);
module_exit(led_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("CFR");
MODULE_INFO(intree, "Y");
