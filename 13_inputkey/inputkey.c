#include <linux/types.h>              /* 提供 u32、size_t 等基础类型定义 */
#include <linux/kernel.h>             /* 提供内核常用宏、打印接口、ARRAY_SIZE 等工具 */
#include <linux/delay.h>              /* 提供延时相关接口，本文件当前未直接使用，但保留为常见驱动头文件 */
#include <linux/init.h>               /* 提供 __init、__exit 等初始化/退出修饰符 */
#include <linux/module.h>             /* 提供模块加载、卸载、MODULE_LICENSE 等模块框架接口 */
#include <linux/of.h>                 /* 提供设备树匹配、of_device_id 等 OF 相关接口 */
#include <linux/platform_device.h>    /* 提供 platform_driver、platform_device 等平台总线接口 */
#include <linux/gpio/consumer.h>      /* 提供 gpiod 描述符风格 GPIO 获取、读值、转中断等接口 */
#include <linux/input.h>              /* 提供 input 子系统核心接口，用于上报按键事件 */
#include <linux/interrupt.h>          /* 提供 request_irq、irqreturn_t、中断处理函数声明 */
#include <linux/timer.h>              /* 提供内核定时器接口，用于按键消抖 */
#include <linux/jiffies.h>            /* 提供 jiffies、msecs_to_jiffies 等时间换算接口 */
#include <linux/slab.h>               /* 提供 devm_kzalloc 等内存分配接口 */

#define GPIOKEY_NAME          "key_input" /* input 设备名称，用户空间会在输入设备信息里看到这个名字 */
#define INPUTKEY_MAX_KEYS     3           /* 当前驱动支持的最大按键数量，这里按你的需求固定为 3 个 */
#define INPUTKEY_DEBOUNCE_MS  15          /* 软件消抖时间，单位毫秒；按键抖动通常出现在按下/松开的前几毫秒 */

/*
 * 这个数组用于定义“第几个 GPIO”对应上报成“哪个 Linux 按键码”。
 * 这里选择 KEY_1 / KEY_2 / KEY_3，原因是最直观，测试时容易看出来。
 * 如果你后面想把它们改成 KEY_ENTER、KEY_ESC、KEY_WAKEUP，也只需要改这里。
 */
static const unsigned short inputkey_keycodes[INPUTKEY_MAX_KEYS] = {
	KEY_1, /* 第 0 个按键上报成键值 KEY_1 */
	KEY_2, /* 第 1 个按键上报成键值 KEY_2 */
	KEY_3, /* 第 2 个按键上报成键值 KEY_3 */
};

/* 前向声明：先声明总设备结构体，后面单个按键结构里需要回指父设备 */
struct inputkey_dev;

/*
 * 单个按键的运行时描述结构体。
 * 这个结构体的作用是：把“一个 GPIO 按键”相关的所有资源收拢在一起管理，
 * 包括 GPIO 描述符、中断号、消抖定时器、上一次稳定状态、所属父设备等。
 */
struct inputkey_button {
	struct inputkey_dev *parent;      /* 指回父设备，定时器和中断回调里需要通过它访问 input 设备 */
	struct gpio_desc *gpiod;          /* 当前按键对应的 GPIO 描述符，负责读引脚电平和转 IRQ */
	struct timer_list timer;          /* 当前按键独立的消抖定时器，避免三个按键互相影响 */
	int irq;                          /* 当前按键对应的 Linux IRQ 号，由 gpiod_to_irq() 转换得到 */
	int last_state;                   /* 上一次稳定状态：0 表示松开，1 表示按下，-1 表示尚未初始化 */
	unsigned short keycode;           /* 当前按键向 input 子系统上报时使用的键值，比如 KEY_1 */
	unsigned int index;               /* 当前按键在数组中的序号，主要用于日志打印和问题定位 */
	bool timer_initialized;           /* 标记当前按键的定时器是否已经完成 timer_setup，便于失败回滚时安全删除 */
	bool irq_requested;               /* 标记当前按键的 IRQ 是否已经申请成功，便于 remove/回滚时安全关闭 */
};

/*
 * 整个按键驱动的私有数据结构体。
 * 这个结构体的作用是：描述整个 platform 设备实例的资源，
 * 包括 input 设备对象、按键总数、三个按键的数组以及注册状态。
 */
struct inputkey_dev {
	struct device *dev;                              /* 保存 &pdev->dev，方便统一打印日志和做资源关联 */
	struct input_dev *input;                         /* input 子系统设备对象，负责向用户空间输出按键事件 */
	unsigned int key_count;                          /* 当前设备树里实际解析到的按键数量 */
	bool input_registered;                           /* 标记 input 设备是否已经成功注册，remove 时据此决定是否注销 */
	struct inputkey_button buttons[INPUTKEY_MAX_KEYS]; /* 3 个按键的数组，每个元素管理一个按键的资源 */
};

/*
 * inputkey_stop_all_buttons - 关闭所有按键中断并同步删除所有消抖定时器
 * @inputkey: 驱动私有数据
 *
 * 作用：
 * 1. 在 remove 或 probe 失败回滚时统一停止所有按键活动。
 * 2. 先关中断，再删定时器，防止定时器刚删完又被新的中断重新启动。
 */
static void inputkey_stop_all_buttons(struct inputkey_dev *inputkey)
{
	unsigned int i;                                              /* 循环变量，用于遍历每一个按键 */

	for (i = 0; i < inputkey->key_count; i++) {                  /* 第一轮先关闭所有已经申请成功的 IRQ */
		if (inputkey->buttons[i].irq_requested &&               /* 必须确认当前按键的 IRQ 确实申请成功过 */
		    inputkey->buttons[i].irq > 0)                      /* 同时 IRQ 号本身必须是有效正数 */
			disable_irq(inputkey->buttons[i].irq);              /* 关闭该按键中断，阻止后续新的中断进入 */
	}

	for (i = 0; i < inputkey->key_count; i++)                    /* 第二轮再同步删除所有按键的定时器 */
		if (inputkey->buttons[i].timer_initialized)             /* 只有真正初始化过的定时器才能安全删除 */
			del_timer_sync(&inputkey->buttons[i].timer);        /* 等待可能正在执行的回调彻底退出 */
}

/*
 * inputkey_timer_func - 按键消抖定时器回调函数
 * @t: 内核传入的定时器对象指针，实际对应某一个按键的 timer 成员
 *
 * 作用：
 * 1. 在中断触发后的 15ms 再次读取 GPIO 电平，过滤机械按键抖动。
 * 2. 如果当前稳定电平与上一次稳定状态不同，则向 input 子系统上报一次按下/松开事件。
 * 3. 最后重新使能该按键中断，让后续边沿还能继续进入中断。
 *
 * 为什么定时器里要“再次读 GPIO”：
 * 因为按键刚按下或刚松开时，电平通常会在几毫秒内来回跳变。
 * 中断只负责“发现有变化”，真正确认最终状态要在延时之后完成。
 */
static void inputkey_timer_func(struct timer_list *t)
{
	struct inputkey_button *button = from_timer(button, t, timer); /* 通过 timer 成员反推出所属按键结构体 */
	struct inputkey_dev *inputkey = button->parent;                /* 取出父设备，后面上报 input 事件要用 */
	int state;                                                    /* 保存本次消抖后读到的稳定逻辑电平 */

	/*
	 * 读取 GPIO 当前逻辑值。
	 * 这里拿到的是“逻辑值”而不是“物理电平”：
	 * 如果设备树里写了 GPIO_ACTIVE_LOW，那么按下时这里会读到 1，松开时读到 0。
	 * 如果设备树里写了 GPIO_ACTIVE_HIGH，那么按下时读到 1，松开时同样读到 0。
	 * 也就是说，驱动不需要自己再反转电平，gpiod 框架已经帮你做了极性转换。
	 */
	state = gpiod_get_value(button->gpiod);
	if (state < 0) {                               /* 小于 0 表示读取失败，常见是底层控制器返回错误码 */
		dev_err(inputkey->dev,                    /* 打印错误日志，便于排查具体是哪个按键读值失败 */
			"key%u: failed to read gpio value, error=%d\n",
			button->index, state);
		enable_irq(button->irq);                  /* 读值失败也必须重新开中断，否则该按键后面会彻底失效 */
		return;                                   /* 本次消抖失败，直接返回 */
	}

	if (state != button->last_state) {            /* 只有状态真的发生变化时才上报，避免重复上报同一状态 */
		button->last_state = state;               /* 先更新缓存状态，表示这次读到的值已经是新的稳定状态 */

		input_report_key(inputkey->input,         /* 向 input 子系统上报一个按键事件 */
				 button->keycode,         /* 上报哪个键值，例如 KEY_1 / KEY_2 / KEY_3 */
				 state);                  /* 上报的状态：1 表示按下，0 表示松开 */
		input_sync(inputkey->input);              /* 立即同步，把本次事件打包提交给 input 核心层 */
	}

	enable_irq(button->irq);                      /* 消抖处理结束后重新打开该按键中断，准备接收下一次边沿 */
}

/*
 * inputkey_irq_handler - GPIO 按键中断处理函数
 * @irq: 触发当前中断的 IRQ 号
 * @dev_id: request_irq 时传入的私有数据，这里我们传的是 struct inputkey_button *
 *
 * 作用：
 * 1. 中断一触发，先临时关闭当前按键中断，避免抖动期间反复重入。
 * 2. 启动/刷新一个 15ms 的消抖定时器。
 * 3. 真实按键状态不在中断里直接判断，而是延迟到定时器回调里完成。
 *
 * 返回值：
 * - IRQ_HANDLED：表示这个中断已经被当前驱动处理
 * - IRQ_NONE：表示传入参数异常，本次中断不归当前驱动处理
 */
static irqreturn_t inputkey_irq_handler(int irq, void *dev_id)
{
	struct inputkey_button *button = dev_id;                          /* 取回 request_irq 传入的按键私有数据 */

	if (!button)                                                     /* 理论上不会为空，这里做防御性判断 */
		return IRQ_NONE;                                             /* 传入数据异常时，告诉内核当前驱动未处理该中断 */

	disable_irq_nosync(irq);                                         /* 先关闭当前 IRQ，避免抖动导致中断风暴 */
	mod_timer(&button->timer,                                        /* 启动或刷新消抖定时器 */
		  jiffies + msecs_to_jiffies(INPUTKEY_DEBOUNCE_MS)); /* 15ms 后执行，时间单位由毫秒转换成 jiffies */

	return IRQ_HANDLED;                                              /* 告诉内核当前中断已经处理完成 */
}

/*
 * inputkey_setup_one_button - 初始化一个按键的 GPIO / IRQ / 定时器资源
 * @pdev: platform 设备指针，用于获取 devm 资源和打印日志
 * @inputkey: 整个驱动的私有数据
 * @index: 当前要初始化的是第几个按键，范围为 0~2
 *
 * 作用：
 * 1. 从设备树的 key-gpios 里按索引获取一个 GPIO 描述符。
 * 2. 为该 GPIO 转换出中断号。
 * 3. 初始化该按键自己的消抖定时器。
 * 4. 申请该按键的中断，并把私有数据绑定成当前 button。
 *
 * 返回值：
 * - 0：初始化成功
 * - 负数错误码：初始化失败，probe 会据此终止
 */
static int inputkey_setup_one_button(struct platform_device *pdev,
				     struct inputkey_dev *inputkey,
				     unsigned int index)
{
	struct inputkey_button *button = &inputkey->buttons[index]; /* 取出当前索引对应的按键结构体，后续都初始化它 */
	int state;                                                 /* 保存初始逻辑状态，用于给 last_state 赋初值 */
	int ret;                                                   /* 保存各步骤返回值，统一用于错误处理 */

	button->parent = inputkey;                                 /* 建立父子关联，回调函数需要通过它访问 input 设备 */
	button->index = index;                                     /* 保存当前按键序号，后面日志打印时便于定位 */
	button->keycode = inputkey_keycodes[index];                /* 根据索引给当前按键分配一个 Linux 键值 */
	button->irq = -1;                                          /* 先把 IRQ 号置成无效值，避免失败回滚时误判 */
	button->last_state = -1;                                   /* 先标记成未初始化，后面读到真实状态后再覆盖 */

	/*
	 * 从设备树属性 key-gpios 中取出第 index 个 GPIO。
	 * 例如：
	 * key-gpios = <&gpiog 3 GPIO_ACTIVE_LOW>,
	 *             <&gpioh 7 GPIO_ACTIVE_LOW>,
	 *             <&gpioa 0 GPIO_ACTIVE_HIGH>;
	 *
	 * 这里的 "key" 对应属性名前缀 key-gpios；
	 * index 表示取第几个；
	 * GPIOD_IN 表示把这个 GPIO 配成输入方向。
	 */
	button->gpiod = devm_gpiod_get_index(&pdev->dev, "key", index, GPIOD_IN);
	if (IS_ERR(button->gpiod)) {                               /* 如果获取失败，PTR_ERR 可以拿到具体错误码 */
		ret = PTR_ERR(button->gpiod);                          /* 把错误码取出来，方便统一返回 */
		dev_err(&pdev->dev,                                    /* 打印日志时带上按键序号，排查更直接 */
			"key%u: failed to get gpio descriptor, error=%d\n",
			index, ret);
		button->gpiod = NULL;                                 /* 失败后清空指针，避免误用错误指针 */
		return ret;                                           /* 返回错误，让 probe 终止 */
	}

	/*
	 * 读取当前 GPIO 的初始逻辑值。
	 * 这样做的目的，是让 last_state 一开始就和真实状态保持一致，
	 * 避免驱动刚加载时因为状态未知而产生一次错误的“伪按键事件”。
	 */
	state = gpiod_get_value(button->gpiod);
	if (state < 0) {                                           /* 如果连初始值都读失败，则说明这个 GPIO 不可用 */
		dev_err(&pdev->dev,
			"key%u: failed to read initial gpio value, error=%d\n",
			index, state);
		return state;                                          /* 直接返回错误码，中止 probe */
	}
	button->last_state = state;                                /* 缓存当前稳定状态，后续只有状态变化时才上报 */

	/*
	 * 把 GPIO 描述符转换成 IRQ 号。
	 * 这样做的好处是：驱动不必硬编码中断号，也不必手工解析设备树里的 interrupts 属性；
	 * 只要这个 GPIO 控制器支持转中断，gpiod_to_irq() 就会给出正确的 Linux IRQ 号。
	 */
	button->irq = gpiod_to_irq(button->gpiod);
	if (button->irq < 0) {                                     /* 返回负数表示该 GPIO 无法转成中断或底层出错 */
		ret = button->irq;                                      /* 保存错误码，统一返回 */
		dev_err(&pdev->dev,
			"key%u: failed to map gpio to irq, error=%d\n",
			index, ret);
		return ret;                                            /* 没有 IRQ 就无法走中断式按键上报，因此直接失败 */
	}

	timer_setup(&button->timer,                                /* 初始化当前按键的消抖定时器对象 */
		    inputkey_timer_func,                           /* 指定定时器超时后执行的回调函数 */
		    0);                                           /* flags 传 0，表示使用普通定时器即可 */
	button->timer_initialized = true;                         /* 记录该定时器已经初始化完成，失败回滚时就可以安全删除 */

	/*
	 * 申请当前按键的中断。
	 * IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING 表示同时响应上升沿和下降沿，
	 * 这样按下和松开都能捕获。配合消抖定时器，就能完整区分“按下事件”和“松开事件”。
	 *
	 * devm_request_irq 的好处：
	 * 驱动卸载时中断会由 devm 机制自动释放，不需要手工 free_irq()。
	 */
	ret = devm_request_irq(&pdev->dev,                         /* 把中断资源托管给设备 dev，卸载时自动回收 */
			       button->irq,                         /* 要申请的 IRQ 号 */
			       inputkey_irq_handler,               /* 中断触发时调用的处理函数 */
			       IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING, /* 双边沿触发，按下和松开都响应 */
			       "gpio_key",                         /* 中断名字，显示在 /proc/interrupts 等调试信息里 */
			       button);                            /* 传给中断回调的私有数据，这里传当前按键结构体 */
	if (ret) {                                                 /* 非 0 表示申请失败 */
		dev_err(&pdev->dev,
			"key%u: failed to request irq %d, error=%d\n",
			index, button->irq, ret);
		return ret;                                            /* 返回错误，中止 probe */
	}
	button->irq_requested = true;                              /* 标记当前按键 IRQ 已成功申请，remove/回滚时需要关闭 */

	dev_info(&pdev->dev,                                       /* 打印当前按键初始化成功日志 */
		 "key%u: gpio and irq initialized, irq=%d, keycode=%u, init_state=%d\n",
		 index, button->irq, button->keycode, button->last_state);

	return 0;                                                  /* 当前按键初始化完成，返回成功 */
}

/* 设备树匹配表：告诉内核这个驱动匹配 compatible = "alientek,key" 的节点 */
static const struct of_device_id inputkey_of_match[] = {
	{ .compatible = "alientek,key" }, /* 匹配你设备树中的 key 节点 */
	{ }                               /* 匹配表结束标记，必须保留空项 */
};
MODULE_DEVICE_TABLE(of, inputkey_of_match);                   /* 导出 OF 匹配表，支持自动加载模块 */

/*
 * inputkey_probe - platform 驱动的探测函数
 * @pdev: 当前匹配成功的 platform 设备
 *
 * 作用：
 * 1. 为整个驱动分配私有数据结构体。
 * 2. 直接按固定 3 个按键进行初始化，依次从 key-gpios 读取第 0/1/2 项。
 * 3. 分配并配置 input 设备对象。
 * 4. 逐个初始化 3 个按键的 GPIO / IRQ / 定时器。
 * 5. 最后注册 input 设备，让用户空间能够收到按键事件。
 *
 * 返回值：
 * - 0：探测成功
 * - 负数错误码：探测失败，内核不会绑定该驱动
 */
static int inputkey_probe(struct platform_device *pdev)
{
	struct inputkey_dev *inputkey;                            /* 保存整个驱动实例的私有数据 */
	unsigned int i;                                           /* 循环变量，用于依次初始化 3 个按键 */
	int ret;                                                  /* 保存各步骤返回值，统一错误处理 */

	inputkey = devm_kzalloc(&pdev->dev,                       /* 申请并清零驱动私有数据内存 */
				sizeof(*inputkey),                  /* 分配大小就是整个 struct inputkey_dev */
				GFP_KERNEL);                        /* GFP_KERNEL 表示在普通进程上下文中分配 */
	if (!inputkey)                                            /* 返回空指针表示内存分配失败 */
		return -ENOMEM;                                       /* 直接返回“内存不足”错误码 */

	inputkey->dev = &pdev->dev;                               /* 保存设备对象指针，后面日志打印会频繁使用 */
	platform_set_drvdata(pdev, inputkey);                     /* 把私有数据绑到 platform 设备上，remove 时可取回 */

	inputkey->key_count = INPUTKEY_MAX_KEYS;                  /* 当前驱动固定支持 3 个按键，后续循环全部按 3 次执行 */

	/*
	 * 分配 input 设备对象。
	 * 这里故意使用普通 input_allocate_device，而不使用 devm_input_allocate_device。
	 * 原因是你的内核没有 devm_input_register_device，说明 input 设备的“注册/注销”阶段
	 * 需要我们自己完全掌控。这样写在老内核上最稳妥，生命周期最清晰：
	 * 1. probe 失败时，用 input_free_device() 回收“尚未注册”的 input 对象
	 * 2. probe 成功后，remove 时用 input_unregister_device() 注销“已经注册”的 input 对象
	 *
	 * 但 GPIO、IRQ 这些资源继续使用 devm 完全没有问题，因为它们的生命周期接口是完整的。
	 * 这就是“可以部分用 devm，部分手动管理”，关键是每类资源的创建和释放要成对匹配。
	 */
	inputkey->input = input_allocate_device();
	if (!inputkey->input) {                                   /* 返回空表示分配失败 */
		dev_err(&pdev->dev, "failed to allocate input device\n");
		return -ENOMEM;                                       /* 直接返回内存不足 */
	}

	inputkey->input->name = GPIOKEY_NAME;                     /* 给 input 设备设置名字，用户空间可见 */
	inputkey->input->phys = "key/input0";                     /* 设置物理路径字符串，主要用于调试区分设备来源 */
	inputkey->input->dev.parent = &pdev->dev;                 /* 指定父设备，形成正确的设备层级关系 */

	/*
	 * 设置 input 设备支持的事件类型和具体按键码。
	 * EV_KEY 表示这是一个“按键类输入设备”；
	 * input_set_capability 会同时设置 evbit 和 keybit，对代码可读性更好。
	 */
	__set_bit(EV_KEY, inputkey->input->evbit);                /* 声明该 input 设备支持按键事件 */
	for (i = 0; i < inputkey->key_count; i++)                 /* 遍历 3 个按键，逐个声明支持的键值 */
		input_set_capability(inputkey->input,                /* 要设置能力的 input 设备对象 */
				     EV_KEY,                          /* 能力所属的事件类型，这里是按键事件 */
				     inputkey_keycodes[i]);           /* 当前这个按键实际对应的 Linux 键码 */

	/*
	 * 逐个初始化 3 个按键的 GPIO / IRQ / 定时器。
	 * 注意这里把“input 设备注册”放在最后，
	 * 这样如果中途某个按键初始化失败，就不会留下一个已经注册但还没完全准备好的 input 设备。
	 */
	for (i = 0; i < inputkey->key_count; i++) {               /* 依次处理第 0、1、2 个按键 */
		ret = inputkey_setup_one_button(pdev, inputkey, i);   /* 初始化一个按键的全部底层资源 */
		if (ret)                                              /* 只要有一个按键失败，整个 probe 就失败 */
			goto err_stop_buttons;                            /* 回滚已经初始化成功的按键资源，再释放 input 对象 */
	}

	/*
	 * 注册 input 设备。
	 * 你的内核里没有 devm_input_register_device，这没有本质问题，
	 * 只要我们自己保证：
	 * 1. probe 成功后在 remove 里调用 input_unregister_device()
	 * 2. probe 失败前不要过早注册，避免半初始化状态留下脏资源
	 * 就可以和 devm 资源管理机制安全共存。
	 */
	ret = input_register_device(inputkey->input);             /* 把 input 设备正式注册到 input 核心层 */
	if (ret) {                                                /* 返回非 0 表示注册失败 */
		dev_err(&pdev->dev,
			"failed to register input device, error=%d\n",
			ret);
		goto err_stop_buttons;                                /* 先停掉已经打开的中断和定时器，再释放 input 对象 */
	}
	inputkey->input_registered = true;                        /* 标记注册成功，remove 时据此决定是否注销 */

	dev_info(&pdev->dev,                                      /* 打印 probe 成功日志，便于确认最终状态 */
		 "gpiokey input driver probe success, key_count=%u\n",
		 inputkey->key_count);
	return 0;                                                 /* 返回 0，表示 probe 全部完成 */

err_stop_buttons:
	inputkey_stop_all_buttons(inputkey);                      /* 停止已经初始化成功的按键活动，避免失败回滚时有并发 */
	input_free_device(inputkey->input);                       /* 释放尚未注册成功的 input 设备对象 */
	inputkey->input = NULL;                                   /* 置空指针，防止后续误用 */
	return ret;                                               /* 把原始错误码返回给上层 */
}

/*
 * inputkey_remove - platform 驱动的卸载函数
 * @pdev: 当前要卸载的 platform 设备
 *
 * 作用：
 * 1. 先禁用全部按键中断，防止卸载过程中还有新中断进入。
 * 2. 再同步删除全部消抖定时器，确保没有回调还在运行。
 * 3. 最后注销 input 设备，把该输入设备从系统中移除。
 *
 * 返回值：
 * - 0：卸载完成
 */
static int inputkey_remove(struct platform_device *pdev)
{
	struct inputkey_dev *inputkey = platform_get_drvdata(pdev); /* 取回 probe 时保存的私有数据 */

	if (!inputkey)                                               /* 理论上不会为空，这里是防御性判断 */
		return 0;                                                /* 没有私有数据就直接返回 */

	inputkey_stop_all_buttons(inputkey);                         /* 统一关闭全部中断并删除全部定时器 */

	if (inputkey->input_registered) {                            /* 只有真正注册过 input 设备才需要注销 */
		input_unregister_device(inputkey->input);               /* 注销 input 设备，释放注册阶段占用的资源 */
		inputkey->input_registered = false;                     /* 清除标记，避免后续误判 */
		inputkey->input = NULL;                                 /* 置空指针，表示该对象已交给 input 核心释放 */
	}

	dev_info(&pdev->dev, "gpiokey input driver removed\n");      /* 打印卸载完成日志 */
	return 0;                                                    /* 返回 0，表示 remove 执行完成 */
}

/*
 * platform 驱动结构体。
 * 这个结构体告诉内核：
 * 1. 这个驱动的名字是什么
 * 2. 设备树该匹配哪些 compatible
 * 3. 匹配成功后调用哪个 probe
 * 4. 卸载时调用哪个 remove
 */
static struct platform_driver inputkey_driver = {
	.probe  = inputkey_probe,                                  /* 当设备树节点匹配成功时执行探测函数 */
	.remove = inputkey_remove,                                 /* 当驱动卸载或设备解绑时执行清理函数 */
	.driver = {
		.name = "atk-key",                                 /* platform 驱动名字，主要用于内核内部标识 */
		.of_match_table = inputkey_of_match,               /* 指向设备树匹配表，实现 OF 自动匹配 */
	},
};

/*
 * inputkey_init - 模块加载入口函数
 *
 * 作用：
 * 把 platform 驱动注册到内核里，之后内核会自动去匹配设备树节点。
 *
 * 返回值：
 * - 0：注册成功
 * - 负数错误码：注册失败
 */
static int __init inputkey_init(void)
{
	return platform_driver_register(&inputkey_driver);         /* 向 platform 总线注册当前驱动 */
}

/*
 * inputkey_exit - 模块卸载出口函数
 *
 * 作用：
 * 把当前 platform 驱动从内核注销，触发对应设备的 remove 清理流程。
 */
static void __exit inputkey_exit(void)
{
	platform_driver_unregister(&inputkey_driver);              /* 从 platform 总线注销当前驱动 */
}

module_init(inputkey_init);                                    /* 指定模块加载时执行的入口函数 */
module_exit(inputkey_exit);                                    /* 指定模块卸载时执行的出口函数 */

MODULE_LICENSE("GPL");                                         /* 声明模块许可证，GPL 才能使用大量内核导出符号 */
MODULE_AUTHOR("CFR");                                          /* 声明模块作者信息 */
MODULE_DESCRIPTION("Three-key GPIO input subsystem driver");   /* 声明模块功能描述 */
MODULE_INFO(intree, "Y");                                      /* 保留原有 intree 标记，表明按树内风格编写 */
