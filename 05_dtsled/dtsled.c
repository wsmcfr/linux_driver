#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/fs.h>          
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/cdev.h>
#define DTSLED_CNT 1
#define DTSLED_NAME "dtsled"
#define LEDOFF 0
#define LEDON  1
//设备结构体
struct dtsled_dev
{
	/* data */
	dev_t devid; // 设备号
	int major; // 主设备号
	int minor; // 次设备号
	struct cdev cdev; // 字符设备
	struct class *class; // 类
	struct device *device; // 设备
	struct device_node	*nd; /* 设备节点 */
	void __iomem *MP_AHB4ENSETR;
	void __iomem *GPIOI_MODER;
	void __iomem *GPIOI_OTYPER;
	void __iomem *GPIOI_OSPEEDR;
	void __iomem *GPIOI_PUPDR;
	void __iomem *GPIOI_BSRR;
};
static struct dtsled_dev dtsled;

/* LED 开关函数，按 PI0 处理 */
static void dtsled_switch(u8 sta)
{
	if (!dtsled.GPIOI_BSRR)
	return;
	if (sta == LEDON) {
		/* 输出低电平，点亮 LED */
		writel((1 << 16), dtsled.GPIOI_BSRR);
	} else if (sta == LEDOFF) {
		/* 输出高电平，熄灭 LED */
		writel((1 << 0), dtsled.GPIOI_BSRR);
	}
}

static int dtsled_open(struct inode *inode, struct file *filp)
{
	filp->private_data = &dtsled;
	return 0;
}

static int dtsled_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static ssize_t dtsled_write(struct file *filp, const char __user *buf, size_t count, loff_t *ppos)
{
	int ret;
	unsigned char databuf[1];
	unsigned char ledstat;


	if (count < 1)
		return -EINVAL;

	ret = copy_from_user(databuf, buf, 1);
	if (ret)
		return -EFAULT;

	ledstat = databuf[0];

	if (ledstat == LEDON)
		dtsled_switch(LEDON);
	else if (ledstat == LEDOFF)
		dtsled_switch(LEDOFF);
	else
		return -EINVAL;

	return 1;
}

/*
 * 设备操作函数结构体
 */
static struct file_operations dtsled_fops = {
	.owner   = THIS_MODULE,
	.open    = dtsled_open,
	.write   = dtsled_write,
	.release = dtsled_release,
};

static int __init dtsled_init(void)
{
	int ret = 0;
	const char *str;
	u32 regdata[12];
	u32 val = 0;

	dtsled.major = 0;
	dtsled.class = NULL;
	dtsled.device = NULL;
	dtsled.nd = NULL;
	dtsled.MP_AHB4ENSETR = NULL;
	dtsled.GPIOI_MODER   = NULL;
	dtsled.GPIOI_OTYPER  = NULL;
	dtsled.GPIOI_OSPEEDR = NULL;
	dtsled.GPIOI_PUPDR   = NULL;
	dtsled.GPIOI_BSRR    = NULL;

	/* 1. 申请设备号 */
	if (dtsled.major) {
		dtsled.devid = MKDEV(dtsled.major, 0);
		ret = register_chrdev_region(dtsled.devid, DTSLED_CNT, DTSLED_NAME);
		if (ret < 0) {
			printk("dtsled chrdev region failed\r\n");
			goto fail_dtsled_region;
		}
	} else {
		ret = alloc_chrdev_region(&dtsled.devid, 0, DTSLED_CNT, DTSLED_NAME);
		if (ret < 0) {
			printk("dtsled chrdev region failed\r\n");
			goto fail_dtsled_region;
		}
		dtsled.major = MAJOR(dtsled.devid);
		dtsled.minor = MINOR(dtsled.devid);
		printk("dtsled major=%d, minor=%d\r\n", dtsled.major, dtsled.minor);
	}

	/* 2. 添加 cdev */
	cdev_init(&dtsled.cdev, &dtsled_fops);
	dtsled.cdev.owner = THIS_MODULE;
	ret = cdev_add(&dtsled.cdev, dtsled.devid, DTSLED_CNT);
	if (ret < 0) {
		printk("dtsled add cdev failed\r\n");
		goto fail_dtsled_cdev;
	}

	/* 3. 创建类 */
	dtsled.class = class_create(THIS_MODULE, DTSLED_NAME);
	if (IS_ERR(dtsled.class)) {
		printk("dtsled create class failed\r\n");
		ret = PTR_ERR(dtsled.class);
		dtsled.class = NULL;
		goto fail_dtsled_class;
	}

	/* 4. 创建设备 */
	dtsled.device = device_create(dtsled.class, NULL, dtsled.devid, NULL, DTSLED_NAME);
	if (IS_ERR(dtsled.device)) {
		printk("dtsled create device failed\r\n");
		ret = PTR_ERR(dtsled.device);
		dtsled.device = NULL;
		goto fail_dtsled_device;
	}

	/* 5. 查找设备树节点 */
	dtsled.nd = of_find_node_by_path("/alphaled");
	if (dtsled.nd == NULL) {
		printk("dtsled node can not found\r\n");
		ret = -EINVAL;
		goto fail_dtsled_findnd;
	}
	printk("dtsled node has been found\r\n");

	/* 6. 读取 status */
	ret = of_property_read_string(dtsled.nd, "status", &str);
	if (ret < 0) {
		printk("dtsled read status failed\r\n");
		goto fail_dtsled_rs;
	}
	printk("dtsled status = %s\r\n", str);

	/* 7. 读取 compatible */
	ret = of_property_read_string(dtsled.nd, "compatible", &str);
	if (ret < 0) {
		printk("dtsled read compatible failed\r\n");
		goto fail_dtsled_rs;
	}
	printk("dtsled compatible = %s\r\n", str);

	/* 8. 读取 reg */
	ret = of_property_read_u32_array(dtsled.nd, "reg", regdata, 12);
	if (ret < 0) {
		printk("dtsled read reg failed\r\n");
		goto fail_dtsled_rs;
	} else {
		int i;
		printk("dtsled reg:\r\n");
		for (i = 0; i < 12; i++)
			printk("regdata[%d] = 0x%08x\r\n", i, regdata[i]);
	}

	/* 9. ioremap: 从设备树 reg 中映射寄存器 */
	dtsled.MP_AHB4ENSETR = of_iomap(dtsled.nd, 0);
	dtsled.GPIOI_MODER   = of_iomap(dtsled.nd, 1);
	dtsled.GPIOI_OTYPER  = of_iomap(dtsled.nd, 2);
	dtsled.GPIOI_OSPEEDR = of_iomap(dtsled.nd, 3);
	dtsled.GPIOI_PUPDR   = of_iomap(dtsled.nd, 4);
	dtsled.GPIOI_BSRR    = of_iomap(dtsled.nd, 5);

	if (!dtsled.MP_AHB4ENSETR || !dtsled.GPIOI_MODER || !dtsled.GPIOI_OTYPER ||
	    !dtsled.GPIOI_OSPEEDR || !dtsled.GPIOI_PUPDR || !dtsled.GPIOI_BSRR) {
		printk("of_iomap failed\r\n");
		ret = -EINVAL;
		goto fail_map;
	}

	/* 10. GPIO 初始化：按 PI0 配置 */

	/* 10.1 使能 GPIOI 时钟 */
	val = readl(dtsled.MP_AHB4ENSETR);
	val |= (1 << 8);
	writel(val, dtsled.MP_AHB4ENSETR);

	/* 10.2 PI0 配置为通用输出模式 */
	val = readl(dtsled.GPIOI_MODER);
	val &= ~(0x3 << (2 * 0));
	val |=  (0x1 << (2 * 0));
	writel(val, dtsled.GPIOI_MODER);

	/* 10.3 PI0 配置为推挽输出 */
	val = readl(dtsled.GPIOI_OTYPER);
	val &= ~(0x1 << 0);
	writel(val, dtsled.GPIOI_OTYPER);

	/* 10.4 PI0 配置为高速 */
	val = readl(dtsled.GPIOI_OSPEEDR);
	val &= ~(0x3 << (2 * 0));
	val |=  (0x3 << (2 * 0));
	writel(val, dtsled.GPIOI_OSPEEDR);

	/* 10.5 PI0 配置为上拉 */
	val = readl(dtsled.GPIOI_PUPDR);
	val &= ~(0x3 << (2 * 0));
	val |=  (0x1 << (2 * 0));
	writel(val, dtsled.GPIOI_PUPDR);

	/* 10.6 默认熄灭 LED */
	dtsled_switch(LEDOFF);

	printk("dtsled init success\r\n");
	return 0;

fail_map:
	if (dtsled.MP_AHB4ENSETR) {
		iounmap(dtsled.MP_AHB4ENSETR);
		dtsled.MP_AHB4ENSETR = NULL;
	}
	if (dtsled.GPIOI_MODER) {
		iounmap(dtsled.GPIOI_MODER);
		dtsled.GPIOI_MODER = NULL;
	}
	if (dtsled.GPIOI_OTYPER) {
		iounmap(dtsled.GPIOI_OTYPER);
		dtsled.GPIOI_OTYPER = NULL;
	}
	if (dtsled.GPIOI_OSPEEDR) {
		iounmap(dtsled.GPIOI_OSPEEDR);
		dtsled.GPIOI_OSPEEDR = NULL;
	}
	if (dtsled.GPIOI_PUPDR) {
		iounmap(dtsled.GPIOI_PUPDR);
		dtsled.GPIOI_PUPDR = NULL;
	}
	if (dtsled.GPIOI_BSRR) {
		iounmap(dtsled.GPIOI_BSRR);
		dtsled.GPIOI_BSRR = NULL;
	}

fail_dtsled_rs:
fail_dtsled_findnd:
	if (!IS_ERR_OR_NULL(dtsled.device)) {
		device_destroy(dtsled.class, dtsled.devid);
		dtsled.device = NULL;
	}
fail_dtsled_device:
	if (!IS_ERR_OR_NULL(dtsled.class)) {
		class_destroy(dtsled.class);
		dtsled.class = NULL;
	}
fail_dtsled_class:
	cdev_del(&dtsled.cdev);
fail_dtsled_cdev:
	unregister_chrdev_region(dtsled.devid, DTSLED_CNT);
fail_dtsled_region:
	return ret;
}

static void __exit dtsled_exit(void)
{
	if (dtsled.GPIOI_BSRR)
		dtsled_switch(LEDOFF);
	if (dtsled.MP_AHB4ENSETR) {
		iounmap(dtsled.MP_AHB4ENSETR);
		dtsled.MP_AHB4ENSETR = NULL;
	}
	if (dtsled.GPIOI_MODER) {
		iounmap(dtsled.GPIOI_MODER);
		dtsled.GPIOI_MODER = NULL;
	}
	if (dtsled.GPIOI_OTYPER) {
		iounmap(dtsled.GPIOI_OTYPER);
		dtsled.GPIOI_OTYPER = NULL;
	}
	if (dtsled.GPIOI_OSPEEDR) {
		iounmap(dtsled.GPIOI_OSPEEDR);
		dtsled.GPIOI_OSPEEDR = NULL;
	}
	if (dtsled.GPIOI_PUPDR) {
		iounmap(dtsled.GPIOI_PUPDR);
		dtsled.GPIOI_PUPDR = NULL;
	}
	if (dtsled.GPIOI_BSRR) {
		iounmap(dtsled.GPIOI_BSRR);
		dtsled.GPIOI_BSRR = NULL;
	}

	if (!IS_ERR_OR_NULL(dtsled.device) && !IS_ERR_OR_NULL(dtsled.class)) {
		device_destroy(dtsled.class, dtsled.devid);
		dtsled.device = NULL;
	}

	if (!IS_ERR_OR_NULL(dtsled.class)) {
		class_destroy(dtsled.class);
		dtsled.class = NULL;
	}

	cdev_del(&dtsled.cdev);
	unregister_chrdev_region(dtsled.devid, DTSLED_CNT);

	printk("dtsled exit\r\n");
}

module_init(dtsled_init);
module_exit(dtsled_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("CFR");
MODULE_INFO(intree, "Y");