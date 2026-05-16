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
#include <linux/interrupt.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/string.h>
#include <linux/spinlock.h>
#include <linux/wait.h>

/* ====================== 宏定义 ====================== */
#define GPIOKEY_CNT   1                     /* 设备号数量（只创建一个设备） */
#define GPIOKEY_NAME  "gpiokey_platform"    /* 设备名称，在 /dev/ 下出现的节点名 */

/**
 * struct gpiokey_dev - GPIO按键驱动的核心设备结构体
 * 
 * 该结构体集中管理按键设备的所有资源和状态，是驱动的核心数据结构。
 */
struct gpiokey_dev {
    dev_t devid;                    /* 设备号（主设备号 + 次设备号） */
    int major;                      /* 主设备号（动态分配时使用） */
    int minor;                      /* 次设备号 */

    struct cdev cdev;               /* 字符设备结构体 */
    struct class *class;            /* 设备类，用于自动创建设备节点 */
    struct device *device;          /* 设备结构体，对应 /dev/gpiokey_platform */

    struct gpio_desc *key_gpiod;    /* GPIO描述符（推荐的现代GPIO接口） */

    int irqnum;                     /* 中断号，由 GPIO 映射而来 */

    /* ==================== 下半部机制 ==================== */
    struct tasklet_struct tasklet;       /* tasklet（软中断下半部），负责快速调度定时器 */
    struct timer_list debounce_timer;    /* 非阻塞消抖定时器（真正执行消抖逻辑） */

    unsigned char key_state;      /* 当前稳定的按键状态：0 = 松开，1 = 按下 */
    char key_event[16];           /* 按键事件字符串："key_dw"（按下）或 "key_up"（松开） */
    int event_ready;              /* 新事件标志：1表示有新事件可读，0表示无事件 */

    spinlock_t lock;              /* 自旋锁，保护共享数据（中断上下文安全） */
    wait_queue_head_t wait;       /* 等待队列，用于实现阻塞式 read() */
};

static struct gpiokey_dev gpiokey;   /* 全局设备实例（单设备驱动） */

/* ====================== 函数声明 ====================== */
/* 中断上半部处理函数 */
static irqreturn_t gpiokey_irq_handler(int irq, void *dev_id);

/* tasklet 回调函数（软中断上下文） */
static void gpiokey_tasklet_func(unsigned long data);

/* 消抖定时器回调函数（真正进行状态判断和事件生成） */
static void gpiokey_debounce_timer_func(struct timer_list *t);

/* 文件操作函数 */
static int gpiokey_open(struct inode *inode, struct file *filp);
static int gpiokey_release(struct inode *inode, struct file *filp);
static ssize_t gpiokey_read(struct file *filp, char __user *buf,
                            size_t count, loff_t *ppos);

/* ====================== 文件操作集合 ====================== */
/**
 * gpiokey_fops - 字符设备文件操作函数集
 */
static const struct file_operations gpiokey_fops = {
    .owner   = THIS_MODULE,
    .open    = gpiokey_open,
    .read    = gpiokey_read,
    .release = gpiokey_release,
};

/* ====================== 文件操作实现 ====================== */

/**
 * gpiokey_open - 打开设备函数
 * @inode: 设备节点对应的 inode 结构
 * @filp: 文件指针
 *
 * 功能：当用户空间程序 open("/dev/gpiokey_platform") 时被调用。
 *       这里仅把设备结构体指针保存到 filp->private_data，供后续 read/write 使用。
 */
static int gpiokey_open(struct inode *inode, struct file *filp)
{
    filp->private_data = &gpiokey;   /* 将设备私有数据挂到文件指针上 */
    return 0;
}

/**
 * gpiokey_release - 释放（关闭）设备函数
 * @inode: 设备节点对应的 inode 结构
 * @filp: 文件指针
 *
 * 功能：当用户空间 close() 时调用。目前无特殊清理工作。
 */
static int gpiokey_release(struct inode *inode, struct file *filp)
{
    return 0;
}

/**
 * gpiokey_read - 阻塞式读取按键事件
 * @filp: 文件指针
 * @buf: 用户空间缓冲区指针
 * @count: 用户希望读取的字节数
 * @ppos: 文件读写位置（本驱动不支持，忽略）
 *
 * 功能说明：
 *   - 这是一个**事件驱动**的阻塞读接口。
 *   - 当没有按键事件时，进程会进入可中断睡眠，CPU占用率为0。
 *   - 当按键状态发生真实变化（经过消抖确认）时，会唤醒等待的进程。
 *   - 返回字符串 "key_dw"（按下）或 "key_up"（松开），带'\0'结束符。
 *
 * 返回值：
 *   成功返回实际拷贝的字节数（包含'\0'），失败返回负错误码。
 */
static ssize_t gpiokey_read(struct file *filp, char __user *buf,
                            size_t count, loff_t *ppos)
{
    struct gpiokey_dev *dev = filp->private_data;
    unsigned long flags;
    char kbuf[16];
    int len;

    spin_lock_irqsave(&dev->lock, flags);

    /* 如果当前没有新事件，就进入睡眠等待 */
    while (!dev->event_ready) {
        spin_unlock_irqrestore(&dev->lock, flags);

        /* 可中断的等待：被信号打断时返回 -ERESTARTSYS */
        if (wait_event_interruptible(dev->wait, dev->event_ready))
            return -ERESTARTSYS;

        spin_lock_irqsave(&dev->lock, flags);
    }

    /* 有事件：复制给用户空间并清除标志 */
    strscpy(kbuf, dev->key_event, sizeof(kbuf));
    dev->event_ready = 0;
    memset(dev->key_event, 0, sizeof(dev->key_event));

    spin_unlock_irqrestore(&dev->lock, flags);

    len = strlen(kbuf) + 1;        /* 包含字符串结束符 '\0' */


    /* 
    * 关键判断逻辑（改进版）：
    * - 如果用户 count == 0 或连字符串内容都装不下，才返回错误
    * - 你的 eapp count=32 完全足够
    * - 即使别人只给 count=8，也能正常返回带 '\0' 的字符串
    */
    if (count == 0 || count < strlen(kbuf)) {   // 只检查实际字符长度
        return -EINVAL;                         // 缓冲区太小，连 "key_dw" 6个字符都放不下
    }

    /* 实际拷贝的字节数 = min(用户count, 完整len) */
    if (len > count)
        len = count;   // 用户缓冲区不够时，只拷贝部分内容（不带 '\0' 也没关系，用户可自行处理）

    /* 拷贝到用户空间 */
    if (copy_to_user(buf, kbuf, len))
        return -EFAULT;

    return len;   /* 返回实际拷贝给用户的字节数 */
}

/* ====================== 中断与下半部机制 ====================== */

/**
 * gpiokey_irq_handler - 中断上半部（Top Half）
 * @irq: 中断号
 * @dev_id: 注册中断时传入的私有数据（这里是 &gpiokey）
 *
 * 功能：中断发生时立即被调用。
 *       为了尽量减少上半部执行时间，仅调度 tasklet，不做任何耗时操作。
 *       返回 IRQ_HANDLED 表示中断已被正确处理。
 */
static irqreturn_t gpiokey_irq_handler(int irq, void *dev_id)
{
    struct gpiokey_dev *dev = dev_id;
    tasklet_schedule(&dev->tasklet);   /* 调度下半部 tasklet */
    return IRQ_HANDLED;
}

/**
 * gpiokey_tasklet_func - tasklet 软中断下半部
 * @data: 注册 tasklet 时传入的数据（这里是 &gpiokey 的地址）
 *
 * 功能：作为中断下半部，快速启动一个10ms的非阻塞消抖定时器。
 *       真正复杂的消抖和状态判断交给定时器回调完成，避免长时间占用软中断上下文。
 */
static void gpiokey_tasklet_func(unsigned long data)
{
    struct gpiokey_dev *dev = (struct gpiokey_dev *)data;

    /* 启动10ms一次性定时器，实现非阻塞延时消抖 */
    mod_timer(&dev->debounce_timer, jiffies + msecs_to_jiffies(10));
}

/**
 * gpiokey_debounce_timer_func - 消抖定时器回调函数（核心逻辑）
 * @t: 定时器结构体指针
 *
 * 功能：
 *   1. 读取当前GPIO实际电平值
 *   2. 与之前保存的稳定状态比较
 *   3. 如果状态没有真正变化（抖动、噪声），则忽略
 *   4. 如果状态发生真实变化，则更新状态、生成事件、唤醒阻塞的 read()
 *
 * 注意：该函数运行在软中断（timer）上下文，不能睡眠。
 */
static void gpiokey_debounce_timer_func(struct timer_list *t)
{
    struct gpiokey_dev *dev = from_timer(dev, t, debounce_timer);
    int ret;
    unsigned long flags;

    /* 读取当前GPIO电平（推荐使用 gpiod 接口） */
    ret = gpiod_get_value(dev->key_gpiod);
    if (ret < 0)
        return;

    spin_lock_irqsave(&dev->lock, flags);

    /* 状态没有发生真实变化，属于抖动或噪声，直接丢弃 */
    if (ret == dev->key_state) {
        spin_unlock_irqrestore(&dev->lock, flags);
        return;
    }

    /* 状态发生真实变化，更新稳定状态并生成事件 */
    dev->key_state = ret;

    if (ret)    /* 按下（高电平或低电平取决于硬件接法，此处以高电平为按下为例） */
        strscpy(dev->key_event, "key_dw", sizeof(dev->key_event));
    else
        strscpy(dev->key_event, "key_up", sizeof(dev->key_event));

    dev->event_ready = 1;

    /* 唤醒所有在 read() 中阻塞的进程 */
    wake_up_interruptible(&dev->wait);

    spin_unlock_irqrestore(&dev->lock, flags);
}

/* ====================== 平台驱动 probe / remove ====================== */

/**
 * gpiokey_probe - 平台设备驱动探测函数（匹配成功后调用）
 * @pdev: 平台设备指针
 *
 * 功能：完成设备的所有初始化工作，包括：
 *       - GPIO 获取与初始状态读取
 *       - 中断申请
 *       - tasklet 和定时器初始化
 *       - 字符设备注册、类创建、设备节点创建
 */
static int gpiokey_probe(struct platform_device *pdev)
{
    int ret;

    /* ==================== 1. 初始化设备结构体 ==================== */
    memset(&gpiokey, 0, sizeof(gpiokey));
    spin_lock_init(&gpiokey.lock);
    init_waitqueue_head(&gpiokey.wait);

    /* ==================== 2. 获取并配置 GPIO ==================== */
    gpiokey.key_gpiod = gpiod_get(&pdev->dev, "key", GPIOD_IN);
    if (IS_ERR(gpiokey.key_gpiod)) {
        ret = PTR_ERR(gpiokey.key_gpiod);
        dev_err(&pdev->dev, "failed to get key gpio\n");
        return ret;
    }

    /* 读取初始按键状态（probe 阶段可安全使用 cansleep 版本） */
    ret = gpiod_get_value_cansleep(gpiokey.key_gpiod);
    if (ret < 0) {
        dev_err(&pdev->dev, "failed to read initial key state\n");
        goto fail_gpio;
    }
    gpiokey.key_state = ret;   /* 保存初始稳定状态 */

    /* ==================== 3. 初始化下半部机制 ==================== */
    tasklet_init(&gpiokey.tasklet, gpiokey_tasklet_func, (unsigned long)&gpiokey);
    timer_setup(&gpiokey.debounce_timer, gpiokey_debounce_timer_func, 0);

    /* ==================== 4. 获取中断并注册 ==================== */
    gpiokey.irqnum = gpiod_to_irq(gpiokey.key_gpiod);
    if (gpiokey.irqnum < 0) {
        ret = gpiokey.irqnum;
        dev_err(&pdev->dev, "failed to get irq number\n");
        goto fail_gpio;
    }

    ret = request_irq(gpiokey.irqnum, gpiokey_irq_handler,
                      IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
                      "gpiokey_irq", &gpiokey);
    if (ret) {
        dev_err(&pdev->dev, "failed to request irq\n");
        goto fail_gpio;
    }

    /* ==================== 5. 字符设备注册 ==================== */
    ret = alloc_chrdev_region(&gpiokey.devid, 0, GPIOKEY_CNT, GPIOKEY_NAME);
    if (ret < 0) {
        dev_err(&pdev->dev, "failed to alloc chrdev region\n");
        goto fail_irq;
    }

    cdev_init(&gpiokey.cdev, &gpiokey_fops);
    gpiokey.cdev.owner = THIS_MODULE;
    ret = cdev_add(&gpiokey.cdev, gpiokey.devid, GPIOKEY_CNT);
    if (ret < 0) {
        dev_err(&pdev->dev, "failed to add cdev\n");
        goto fail_cdev;
    }

    /* 创建类和设备节点（/dev/gpiokey_platform） */
    gpiokey.class = class_create(THIS_MODULE, GPIOKEY_NAME);
    if (IS_ERR(gpiokey.class)) {
        ret = PTR_ERR(gpiokey.class);
        goto fail_class;
    }

    gpiokey.device = device_create(gpiokey.class, NULL, gpiokey.devid, NULL, GPIOKEY_NAME);
    if (IS_ERR(gpiokey.device)) {
        ret = PTR_ERR(gpiokey.device);
        goto fail_device;
    }

    dev_info(&pdev->dev, "gpiokey platform driver probe success "
             "(tasklet + non-blocking timer debounce + blocking read)\n");
    return 0;

    /* 错误处理路径 */
fail_device:
    class_destroy(gpiokey.class);
fail_class:
    cdev_del(&gpiokey.cdev);
fail_cdev:
    unregister_chrdev_region(gpiokey.devid, GPIOKEY_CNT);
fail_irq:
    free_irq(gpiokey.irqnum, &gpiokey);
    tasklet_kill(&gpiokey.tasklet);
    del_timer_sync(&gpiokey.debounce_timer);
fail_gpio:
    gpiod_put(gpiokey.key_gpiod);
    return ret;
}

/**
 * gpiokey_remove - 平台设备移除函数（卸载驱动或设备移除时调用）
 * @pdev: 平台设备指针
 *
 * 功能：释放所有申请的资源，防止内存泄漏和资源占用。
 */
static int gpiokey_remove(struct platform_device *pdev)
{
    tasklet_kill(&gpiokey.tasklet);
    del_timer_sync(&gpiokey.debounce_timer);
    free_irq(gpiokey.irqnum, &gpiokey);

    device_destroy(gpiokey.class, gpiokey.devid);
    class_destroy(gpiokey.class);
    cdev_del(&gpiokey.cdev);
    unregister_chrdev_region(gpiokey.devid, GPIOKEY_CNT);

    if (gpiokey.key_gpiod)
        gpiod_put(gpiokey.key_gpiod);

    dev_info(&pdev->dev, "gpiokey platform driver removed\n");
    return 0;
}

/* ====================== 设备树匹配表 ====================== */
static const struct of_device_id gpiokey_of_match[] = {
    { .compatible = "atk,key-gpio" },   /* 与设备树中 compatible 属性匹配 */
    { }
};
MODULE_DEVICE_TABLE(of, gpiokey_of_match);

/* ====================== 平台驱动注册 ====================== */
static struct platform_driver gpiokey_driver = {
    .probe  = gpiokey_probe,
    .remove = gpiokey_remove,
    .driver = {
        .name = "atk-gpiokey",
        .of_match_table = gpiokey_of_match,
    },
};

module_platform_driver(gpiokey_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("CFR (optimized)");
MODULE_DESCRIPTION("GPIO Key platform driver: tasklet + non-blocking timer debounce + blocking read");
MODULE_INFO(intree, "Y");