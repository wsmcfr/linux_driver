#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/gpio/consumer.h>
#include <linux/uaccess.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/spinlock.h>
#include <linux/ioctl.h>
#define TIMERLED_CNT   1
#define TIMERLED_NAME  "leddev"

struct timerled_time {
    int on_ms;
    int off_ms;
};

#define TIMERLED_IOC_MAGIC   'L'

#define TIMERLED_CMD_ON      _IO(TIMERLED_IOC_MAGIC, 0)
#define TIMERLED_CMD_OFF     _IO(TIMERLED_IOC_MAGIC, 1)
#define TIMERLED_CMD_BLINK   _IOW(TIMERLED_IOC_MAGIC, 2, int)
#define TIMERLED_CMD_CUSTOM  _IOW(TIMERLED_IOC_MAGIC, 3, struct timerled_time)



/* 驱动设备结构体 */
struct timerled_dev {
    dev_t devid;
    int major;
    int minor;
    struct cdev cdev;
    struct class *class;
    struct device *device;

    struct gpio_desc *led_gpiod;   /* LED GPIO 描述符 */

    struct timer_list timer;       /* 软件定时器 */
    spinlock_t lock;               /* 保护共享数据 */

    int led_state;                 /* 当前 LED 状态: 0灭 1亮 */
    int running;                   /* 定时器是否运行 */
    int on_ms;                     /* 亮时间 */
    int off_ms;                    /* 灭时间 */
};

static struct timerled_dev timerled;

/* 设置 LED 状态 */
static void timerled_set_value(struct timerled_dev *dev, int value)
{

	if(!dev || !dev->led_gpiod)
		return;
    gpiod_set_value(dev -> led_gpiod,value);
}

/* 定时器回调函数 */
static void timerled_timer_function(struct timer_list *t)
{
    struct timerled_dev *dev = from_timer(dev, t, timer);
    unsigned long flags;
    unsigned int next_ms;

    spin_lock_irqsave(&dev->lock, flags);

    if(!dev->running)
    {
        spin_unlock_irqrestore(&dev->lock, flags);
        return;
    }

    if(dev->led_state)
    {
        dev->led_state = 0;
        timerled_set_value(dev,dev->led_state);
        next_ms = dev->off_ms;

    }
    else
    {
        dev->led_state = 1;
        timerled_set_value(dev,dev->led_state);
        next_ms = dev->on_ms;
    }
    
    /* 重新启动定时器 */
    mod_timer(&dev->timer, jiffies + msecs_to_jiffies(next_ms));

    spin_unlock_irqrestore(&dev->lock, flags);
}

/* open */
static int timerled_open(struct inode *inode, struct file *filp)
{
    filp->private_data = &timerled;
    return 0;
}

/* release */
static int timerled_release(struct inode *inode, struct file *filp)
{
    return 0;
}

static long timerled_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct timerled_dev *dev = filp->private_data;
    struct timerled_time time;
    int period;
    unsigned long flags;

    switch (cmd) {
    case TIMERLED_CMD_ON:
        del_timer_sync(&dev->timer);

        spin_lock_irqsave(&dev->lock, flags);
        dev->running = 0;
        dev->led_state = 1;
        timerled_set_value(dev, dev->led_state);
        spin_unlock_irqrestore(&dev->lock, flags);
        break;

    case TIMERLED_CMD_OFF:
        del_timer_sync(&dev->timer);

        spin_lock_irqsave(&dev->lock, flags);
        dev->running = 0;
        dev->led_state = 0;
        timerled_set_value(dev, dev->led_state);
        spin_unlock_irqrestore(&dev->lock, flags);
        break;

    case TIMERLED_CMD_BLINK:
        if (copy_from_user(&period, (void __user *)arg, sizeof(period)))
            return -EFAULT;

        if (period <= 0)
            return -EINVAL;

        spin_lock_irqsave(&dev->lock, flags);
        dev->on_ms = period;
        dev->off_ms = period;
        dev->running = 1;
        dev->led_state = 1;
        timerled_set_value(dev, dev->led_state);
        mod_timer(&dev->timer, jiffies + msecs_to_jiffies(dev->on_ms));
        spin_unlock_irqrestore(&dev->lock, flags);
        break;

    case TIMERLED_CMD_CUSTOM:
        if (copy_from_user(&time, (void __user *)arg, sizeof(time)))
            return -EFAULT;

        if (time.on_ms <= 0 || time.off_ms <= 0)
            return -EINVAL;

        spin_lock_irqsave(&dev->lock, flags);
        dev->on_ms = time.on_ms;
        dev->off_ms = time.off_ms;
        dev->running = 1;
        dev->led_state = 1;
        timerled_set_value(dev, dev->led_state);
        mod_timer(&dev->timer, jiffies + msecs_to_jiffies(dev->on_ms));
        spin_unlock_irqrestore(&dev->lock, flags);
        break;

    default:
        return -EINVAL;
    }

    return 0;
}



// /* write: 接收应用层命令 */
// static ssize_t timerled_write(struct file *filp, const char __user *buf,
//                               size_t count, loff_t *ppos)
// {
//     struct timerled_dev *dev = filp->private_data;
//     struct timerled_cmd cmd;
//     unsigned long flags;

//     if(count != sizeof(cmd))
//         return -EINVAL;
//     if(copy_from_user(&cmd,buf,sizeof(cmd)))
//         return -EINVAL;
//     spin_lock_irqsave(&dev->lock, flags);
//     switch (cmd.cmd)
//     {
//     case CMD_LED_ON:
//         dev->running = 0;
//         del_timer_sync(&dev->timer);
//         dev->led_state = 1;
//         timerled_set_value(dev,dev->led_state);
//         break;
//     case CMD_LED_OFF:
//         dev->running = 0;
//         del_timer_sync(&dev->timer);
//         dev->led_state = 0;
//         timerled_set_value(dev,dev->led_state);       
//         break;
//     case CMD_LED_BLINK:
//         if (cmd.on_ms <= 0) {
//             spin_unlock_irqrestore(&dev->lock, flags);
//             return -EINVAL;
//         }
//         dev->on_ms = cmd.on_ms;
//         dev->off_ms = cmd.on_ms;
//         dev->running = 1;
//         dev->led_state = 1;
//         timerled_set_value(dev,dev->led_state); 
//         mod_timer(&dev->timer, jiffies + msecs_to_jiffies(dev->on_ms));
//         break;    
//     case CMD_LED_CUSTOM:
//         if (cmd.on_ms <= 0 || cmd.off_ms <= 0) {
//             spin_unlock_irqrestore(&dev->lock, flags);
//             return -EINVAL;
//         }
//         dev->on_ms = cmd.on_ms;
//         dev->off_ms = cmd.off_ms;
//         dev->running = 1;
//         dev->led_state = 1;
//         timerled_set_value(dev,dev->led_state); 
//         mod_timer(&dev->timer, jiffies + msecs_to_jiffies(dev->on_ms));
//         break;
//     default:
//         spin_unlock_irqrestore(&dev->lock, flags);
//         return -EINVAL;
//         break;
//     }

//     spin_unlock_irqrestore(&dev->lock, flags);
//     return sizeof(cmd);
// }

static const struct file_operations timerled_fops = {
    .owner          = THIS_MODULE,
    .open           = timerled_open,
    .release        = timerled_release,
    .unlocked_ioctl = timerled_ioctl,
};

/* 设备树匹配 */
static const struct of_device_id timerled_of_match[] = {
    { .compatible = "alientek,led" },
    { }
};
MODULE_DEVICE_TABLE(of, timerled_of_match);

/* probe */
static int timerled_probe(struct platform_device *pdev)
{
    int ret;

    timerled.devid = 0;
    timerled.major = 0;
    timerled.minor = 0;
    timerled.class = NULL;
    timerled.device = NULL;
    timerled.led_gpiod = NULL;
    timerled.led_state = 0;
    timerled.running = 0;
    timerled.on_ms = 500;
    timerled.off_ms = 500;

    spin_lock_init(&timerled.lock);

	/* 获取 LED GPIO，设备树属性名对应 led-gpios */
	timerled.led_gpiod = gpiod_get(&pdev->dev, "led", GPIOD_OUT_LOW);
	if (IS_ERR(timerled.led_gpiod)) {
		ret = PTR_ERR(timerled.led_gpiod);
		dev_err(&pdev->dev, "failed to get led gpio, error = %d\n", ret);
		timerled.led_gpiod = NULL;
		return ret;
	}

	dev_info(&pdev->dev, "successfully got led gpio\n");

    /* 初始化软件定时器 */
    timer_setup(&timerled.timer, timerled_timer_function, 0);

    /* 申请设备号 */
    ret = alloc_chrdev_region(&timerled.devid, 0, TIMERLED_CNT, TIMERLED_NAME);
    if (ret < 0) {
        dev_err(&pdev->dev, "alloc_chrdev_region failed\n");
        goto fail_alloc_chrdev;
    }
    timerled.major = MAJOR(timerled.devid);
    timerled.minor = MINOR(timerled.devid);

    /* 初始化并添加 cdev */
    cdev_init(&timerled.cdev, &timerled_fops);
    timerled.cdev.owner = THIS_MODULE;
    ret = cdev_add(&timerled.cdev, timerled.devid, TIMERLED_CNT);
    if (ret < 0) {
        dev_err(&pdev->dev, "cdev_add failed\n");
        goto fail_cdev_add;
    }

    /* 创建设备类 */
    timerled.class = class_create(THIS_MODULE, TIMERLED_NAME);
    if (IS_ERR(timerled.class)) {
        ret = PTR_ERR(timerled.class);
        timerled.class = NULL;
        dev_err(&pdev->dev, "class_create failed\n");
        goto fail_class_create;
    }

    /* 创建设备节点 /dev/timer_plat */
    timerled.device = device_create(timerled.class, NULL,
                                    timerled.devid, NULL, TIMERLED_NAME);
    if (IS_ERR(timerled.device)) {
        ret = PTR_ERR(timerled.device);
        timerled.device = NULL;
        dev_err(&pdev->dev, "device_create failed\n");
        goto fail_device_create;
    }

    dev_info(&pdev->dev, "timerled probe success\n");
    return 0;

fail_device_create:
    class_destroy(timerled.class);
    timerled.class = NULL;

fail_class_create:
    cdev_del(&timerled.cdev);

fail_cdev_add:
    unregister_chrdev_region(timerled.devid, TIMERLED_CNT);
    timerled.devid = 0;

fail_alloc_chrdev:
    if (timerled.led_gpiod) {
        gpiod_put(timerled.led_gpiod);
        timerled.led_gpiod = NULL;
    }

    return ret;
}

/* remove */
static int timerled_remove(struct platform_device *pdev)
{
    del_timer_sync(&timerled.timer);

    if (!IS_ERR_OR_NULL(timerled.device) && !IS_ERR_OR_NULL(timerled.class)) {
        device_destroy(timerled.class, timerled.devid);
        timerled.device = NULL;
    }

    if (!IS_ERR_OR_NULL(timerled.class)) {
        class_destroy(timerled.class);
        timerled.class = NULL;
    }

    cdev_del(&timerled.cdev);
    unregister_chrdev_region(timerled.devid, TIMERLED_CNT);

    if (timerled.led_gpiod) {
        gpiod_set_value(timerled.led_gpiod, 0);
        gpiod_put(timerled.led_gpiod);
        timerled.led_gpiod = NULL;
    }

    dev_info(&pdev->dev, "timerled removed\n");
    return 0;
}

static struct platform_driver timerled_driver = {
    .probe  = timerled_probe,
    .remove = timerled_remove,
    .driver = {
        .name = "alientek,led",
        .of_match_table = timerled_of_match,
    },
};

static int __init timerled_init(void)
{
    return platform_driver_register(&timerled_driver);
}

static void __exit timerled_exit(void)
{
    platform_driver_unregister(&timerled_driver);
}

module_init(timerled_init);
module_exit(timerled_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("CFR");
MODULE_DESCRIPTION("platform timer led driver");