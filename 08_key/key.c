#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/cdev.h>

#define GPIOKEY_CNT   1
#define GPIOKEY_NAME  "gpiokey_platform"

struct gpiokey_dev
{
	dev_t devid;                 /* 设备号 */
	int major;                  /* 主设备号 */
	int minor;                  /* 次设备号 */
	struct cdev cdev;           /* 字符设备 */
	struct class *class;        /* 设备类 */
	struct device *device;      /* 设备对象 */
	struct gpio_desc *key_gpiod;/* KEY 对应的 GPIO 描述符 */
};

static struct gpiokey_dev gpiokey;

/*
 * 打开设备
 */
static int gpiokey_open(struct inode *inode, struct file *filp)
{
	filp->private_data = &gpiokey;
	return 0;
}

/*
 * 关闭设备
 */
static int gpiokey_release(struct inode *inode, struct file *filp)
{
	return 0;
}

/*
 * read
 * 返回 1 个字节：
 * 0 -> 松开
 * 1 -> 按下
 *
 * 对于设备树里声明为 GPIO_ACTIVE_LOW 的按键：
 * gpiod_get_value_cansleep() 返回的是逻辑值
 *   按下(物理低电平) -> 返回 1
 *   松开(物理高电平) -> 返回 0
 */
static ssize_t gpiokey_read(struct file *filp, char __user *buf,
			    size_t count, loff_t *ppos)
{
	int ret;
	unsigned char key_value;
	struct gpiokey_dev *dev = filp->private_data;

	if (count < 1)
		return -EINVAL;

	/* 读取 GPIO 逻辑值 */
	ret = gpiod_get_value_cansleep(dev->key_gpiod);
	if (ret < 0)
		return ret;

	key_value = (unsigned char)ret;

	if (copy_to_user(buf, &key_value, 1))
		return -EFAULT;

	return 1;
}

/*
 * 文件操作集
 */
static const struct file_operations gpiokey_fops = {
	.owner   = THIS_MODULE,
	.open    = gpiokey_open,
	.read    = gpiokey_read,
	.release = gpiokey_release,
};

/*
 * 设备树匹配表
 */
static const struct of_device_id gpiokey_of_match[] = {
	{ .compatible = "atk,key-gpio" },
	{ }
};
MODULE_DEVICE_TABLE(of, gpiokey_of_match);

/*
 * probe
 */
static int gpiokey_probe(struct platform_device *pdev)
{
	int ret;

	gpiokey.devid = 0;
	gpiokey.major = 0;
	gpiokey.minor = 0;
	gpiokey.class = NULL;
	gpiokey.device = NULL;
	gpiokey.key_gpiod = NULL;

	/*
	 * 从设备树获取 key-gpios
	 * con_id = "key" 对应属性名 key-gpios
	 * GPIOD_IN 表示输入模式
	 */
	gpiokey.key_gpiod = gpiod_get(&pdev->dev, "key", GPIOD_IN);
	if (IS_ERR(gpiokey.key_gpiod)) {
		ret = PTR_ERR(gpiokey.key_gpiod);
		gpiokey.key_gpiod = NULL;
		dev_err(&pdev->dev, "failed to get key gpio\n");
		return ret;
	}

	/* 申请字符设备号 */
	ret = alloc_chrdev_region(&gpiokey.devid, 0, GPIOKEY_CNT, GPIOKEY_NAME);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to alloc chrdev region\n");
		goto fail_chrdev_region;
	}
	gpiokey.major = MAJOR(gpiokey.devid);
	gpiokey.minor = MINOR(gpiokey.devid);

	/* 初始化 cdev 并添加到内核 */
	cdev_init(&gpiokey.cdev, &gpiokey_fops);
	gpiokey.cdev.owner = THIS_MODULE;
	ret = cdev_add(&gpiokey.cdev, gpiokey.devid, GPIOKEY_CNT);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to add cdev\n");
		goto fail_cdev_add;
	}

	/* 创建设备类 */
	gpiokey.class = class_create(THIS_MODULE, GPIOKEY_NAME);
	if (IS_ERR(gpiokey.class)) {
		ret = PTR_ERR(gpiokey.class);
		gpiokey.class = NULL;
		dev_err(&pdev->dev, "failed to create class\n");
		goto fail_class_create;
	}

	/* 创建设备节点 /dev/gpiokey_platform */
	gpiokey.device = device_create(gpiokey.class, NULL,
				       gpiokey.devid, NULL, GPIOKEY_NAME);
	if (IS_ERR(gpiokey.device)) {
		ret = PTR_ERR(gpiokey.device);
		gpiokey.device = NULL;
		dev_err(&pdev->dev, "failed to create device\n");
		goto fail_device_create;
	}

	dev_info(&pdev->dev, "gpiokey platform driver probe success\n");
	return 0;

fail_device_create:
	if (!IS_ERR_OR_NULL(gpiokey.class)) {
		class_destroy(gpiokey.class);
		gpiokey.class = NULL;
	}

fail_class_create:
	cdev_del(&gpiokey.cdev);

fail_cdev_add:
	unregister_chrdev_region(gpiokey.devid, GPIOKEY_CNT);
	gpiokey.devid = 0;

fail_chrdev_region:
	if (gpiokey.key_gpiod) {
		gpiod_put(gpiokey.key_gpiod);
		gpiokey.key_gpiod = NULL;
	}

	return ret;
}

/*
 * remove
 */
static int gpiokey_remove(struct platform_device *pdev)
{
	if (!IS_ERR_OR_NULL(gpiokey.device) && !IS_ERR_OR_NULL(gpiokey.class)) {
		device_destroy(gpiokey.class, gpiokey.devid);
		gpiokey.device = NULL;
	}

	if (!IS_ERR_OR_NULL(gpiokey.class)) {
		class_destroy(gpiokey.class);
		gpiokey.class = NULL;
	}

	cdev_del(&gpiokey.cdev);
	unregister_chrdev_region(gpiokey.devid, GPIOKEY_CNT);

	if (gpiokey.key_gpiod) {
		gpiod_put(gpiokey.key_gpiod);
		gpiokey.key_gpiod = NULL;
	}

	dev_info(&pdev->dev, "gpiokey platform driver removed\n");
	return 0;
}

/*
 * platform_driver
 */
static struct platform_driver gpiokey_driver = {
	.probe  = gpiokey_probe,
	.remove = gpiokey_remove,
	.driver = {
		.name = "atk-gpiokey",
		.of_match_table = gpiokey_of_match,
	},
};

static int __init gpiokey_init(void)
{
	return platform_driver_register(&gpiokey_driver);
}

static void __exit gpiokey_exit(void)
{
	platform_driver_unregister(&gpiokey_driver);
}

module_init(gpiokey_init);
module_exit(gpiokey_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("CFR");
MODULE_DESCRIPTION("GPIO Key platform driver");
MODULE_INFO(intree, "Y");