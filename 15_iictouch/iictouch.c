#include <linux/delay.h>              /* 提供 msleep()，用于 Goodix 触摸 IC 的复位和上电等待。 */
#include <linux/gpio.h>               /* 提供 legacy GPIO 接口，用于课程设备树中的 reset-gpios 和 irq-gpios。 */
#include <linux/i2c.h>                /* 提供 I2C client、i2c_transfer() 等接口，GT911/GT9147 通过 I2C 通信。 */
#include <linux/input.h>              /* 提供 input 子系统接口，触摸坐标最终通过 input 事件上报。 */
#include <linux/input/mt.h>           /* 提供多点触摸 slot 接口，用于上报 ABS_MT_POSITION_X/Y。 */
#include <linux/interrupt.h>          /* 提供线程化中断接口，I2C 读写必须放在线程中执行。 */
#include <linux/module.h>             /* 提供内核模块宏，例如 module_i2c_driver() 和 MODULE_LICENSE。 */
#include <linux/of.h>                 /* 提供设备树通用读取接口，用于读取 touchscreen-size-x/y。 */
#include <linux/of_gpio.h>            /* 提供 of_get_named_gpio()，用于读取设备树 GPIO。 */
#include <linux/slab.h>               /* 提供 devm_kzalloc()，用于分配驱动私有数据。 */

#define GOODIX_CTRL_REG		0x8040 /* Goodix 控制寄存器，官方例程用于软件复位，本驱动只读取触摸数据。 */
#define GOODIX_CFG_REG		0x8047 /* Goodix 配置起始寄存器，可读取配置版本，但不自动覆盖芯片配置。 */
#define GOODIX_PID_REG		0x8140 /* Goodix 产品 ID 寄存器，GT911 会返回 ASCII 字符串 "911"。 */
#define GOODIX_STATUS_REG	0x814e /* Goodix 触摸状态寄存器，bit7 表示有新数据，bit3:0 表示触点数。 */
#define GOODIX_POINT_REG	0x814f /* Goodix 第一个触点数据起始寄存器，每个触点占 8 字节。 */

#define GOODIX_STATUS_READY	BIT(7) /* 触摸状态寄存器 bit7，置 1 表示控制器准备好了新一帧数据。 */
#define GOODIX_POINT_SIZE	8 /* Goodix 每个触点数据长度：ID、X、Y、触摸面积等，共 8 字节。 */
#define GOODIX_MAX_POINTS	5 /* 当前 RGBLCD 触摸测试最多按 5 点处理，和官方例程保持一致。 */
#define GOODIX_DATA_LEN		(1 + GOODIX_POINT_SIZE * GOODIX_MAX_POINTS) /* 一次读取状态字节和最多 5 个触点。 */

#define GOODIX_DEFAULT_MAX_X	1024 /* 默认 LCD 宽度，当前 RGBLCD 使用 1024 像素。 */
#define GOODIX_DEFAULT_MAX_Y	600 /* 默认 LCD 高度，当前 RGBLCD 使用 600 像素。 */

#define GOODIX_RESET_LOW_MS	20 /* RESET 拉低保持时间，保证触摸 IC 进入复位状态。 */
#define GOODIX_RESET_HIGH_MS	80 /* RESET 拉高后的等待时间，保证触摸 IC 固件启动完成。 */
#define GOODIX_INT_READY_MS	50 /* INT 由输出切到输入后的等待时间，保证中断脚状态稳定。 */

/*
 * struct goodix_ts_data - GT911/GT9147 触摸驱动私有数据。
 * @client: 当前 I2C 设备对象，保存 I2C 地址和设备树节点。
 * @input: 注册到 input 子系统的输入设备，用户空间通过 /dev/input/eventX 读取它。
 * @max_x: X 轴坐标最大范围，默认 1024，可由设备树 touchscreen-size-x 覆盖。
 * @max_y: Y 轴坐标最大范围，默认 600，可由设备树 touchscreen-size-y 覆盖。
 * @reset_gpio: 设备树 reset-gpios 对应的 GPIO 编号，用于硬件复位。
 * @irq_gpio: 设备树 irq-gpios 或 interrupt-gpios 对应的 GPIO 编号，用于触摸中断。
 * @irq: 最终申请到的 Linux IRQ 编号，可能来自 client->irq，也可能来自 gpio_to_irq()。
 * @slot_active: 记录每个 slot 是否已经按下，用于触点消失或 I2C 出错时补发抬起事件。
 */
struct goodix_ts_data {
	struct i2c_client *client;                         /* 保存 I2C client，所有寄存器读写都依赖它。 */
	struct input_dev *input;                           /* 保存 input 设备指针，中断线程通过它上报触摸事件。 */
	u32 max_x;                                         /* 保存 X 轴坐标范围上限，input 坐标范围为 0 到 max_x - 1。 */
	u32 max_y;                                         /* 保存 Y 轴坐标范围上限，input 坐标范围为 0 到 max_y - 1。 */
	int reset_gpio;                                    /* 保存复位 GPIO 编号，GPIO 无效时为负数。 */
	int irq_gpio;                                      /* 保存中断 GPIO 编号，GPIO 无效时为负数。 */
	int irq;                                           /* 保存最终使用的 Linux IRQ 编号。 */
	bool slot_active[GOODIX_MAX_POINTS];               /* 保存每个触摸 slot 当前是否处于按下状态。 */
};

/*
 * goodix_read_regs - 从 Goodix 触摸 IC 连续读取寄存器。
 * @ts: 驱动私有数据，函数通过 ts->client 找到 I2C 设备。
 * @reg: 16 位寄存器地址，Goodix 寄存器地址高字节先发送。
 * @buf: 保存读取结果的缓冲区。
 * @len: 要读取的字节数。
 *
 * 主要流程：
 * 1. 先通过 I2C 写入 2 字节寄存器地址。
 * 2. 再通过 repeated start 连续读出 len 字节数据。
 *
 * 返回值：
 * 0 表示读取成功；负数表示 I2C 传输失败。
 */
static int goodix_read_regs(struct goodix_ts_data *ts, u16 reg, u8 *buf, int len)
{
	struct i2c_client *client = ts->client;             /* 取出 I2C client，后续访问地址和适配器都需要它。 */
	u8 regbuf[2];                                       /* 保存 Goodix 16 位寄存器地址，高字节在前。 */
	struct i2c_msg msgs[2];                             /* 定义两个 I2C 消息：先写寄存器地址，再读数据。 */
	int ret;                                            /* 保存 i2c_transfer() 返回值。 */

	regbuf[0] = reg >> 8;                                /* 保存寄存器地址高 8 位。 */
	regbuf[1] = reg & 0xff;                              /* 保存寄存器地址低 8 位。 */

	msgs[0].addr = client->addr;                         /* 第一个消息访问设备树 reg 指定的 I2C 地址。 */
	msgs[0].flags = 0;                                   /* flags 为 0 表示写操作。 */
	msgs[0].len = sizeof(regbuf);                        /* 写入 2 字节寄存器地址。 */
	msgs[0].buf = regbuf;                                /* 写缓冲区指向寄存器地址数组。 */

	msgs[1].addr = client->addr;                         /* 第二个消息继续访问同一个 Goodix 设备。 */
	msgs[1].flags = I2C_M_RD;                            /* I2C_M_RD 表示读操作。 */
	msgs[1].len = len;                                   /* 读取调用者指定的字节数。 */
	msgs[1].buf = buf;                                   /* 读出的数据保存到调用者缓冲区。 */

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs)); /* 执行组合 I2C 传输。 */
	if (ret == ARRAY_SIZE(msgs))                         /* 两个消息都完成时表示读取成功。 */
		return 0;                                      /* 返回 0 给调用者。 */

	if (ret < 0)                                        /* 负数表示 I2C 控制器返回了明确错误。 */
		return ret;                                    /* 保留底层错误码，便于 dmesg 定位。 */

	return -EIO;                                        /* 非负但消息数不足，说明传输不完整。 */
}

/*
 * goodix_write_regs - 向 Goodix 触摸 IC 连续写入寄存器。
 * @ts: 驱动私有数据，函数通过 ts->client 找到 I2C 设备。
 * @reg: 16 位寄存器地址，Goodix 寄存器地址高字节先发送。
 * @buf: 要写入的数据缓冲区。
 * @len: 要写入的数据长度。
 *
 * 返回值：
 * 0 表示写入成功；负数表示写入失败。
 */
static int goodix_write_regs(struct goodix_ts_data *ts, u16 reg, const u8 *buf, int len)
{
	u8 txbuf[2 + 16];                                    /* 保存寄存器地址和少量写入数据，本驱动只写 1 字节状态清除。 */
	struct i2c_msg msg;                                  /* Goodix 写寄存器只需要一个 I2C 写消息。 */
	int ret;                                             /* 保存 i2c_transfer() 返回值。 */

	if (len < 0 || len > 16)                              /* 防止调用者写入超过栈缓冲区容量的数据。 */
		return -EINVAL;                                 /* 返回参数错误，避免栈内存越界。 */

	txbuf[0] = reg >> 8;                                  /* 保存寄存器地址高 8 位。 */
	txbuf[1] = reg & 0xff;                                /* 保存寄存器地址低 8 位。 */
	memcpy(&txbuf[2], buf, len);                          /* 把要写入的数据拼接到寄存器地址之后。 */

	msg.addr = ts->client->addr;                          /* 写消息访问当前 Goodix I2C 地址。 */
	msg.flags = 0;                                        /* flags 为 0 表示写操作。 */
	msg.len = len + 2;                                    /* 写入总长度等于 2 字节地址加数据长度。 */
	msg.buf = txbuf;                                      /* 写缓冲区指向拼接好的地址和数据。 */

	ret = i2c_transfer(ts->client->adapter, &msg, 1);      /* 执行 I2C 写传输。 */
	if (ret == 1)                                         /* 返回 1 表示一个写消息完整完成。 */
		return 0;                                       /* 返回 0，表示写入成功。 */

	if (ret < 0)                                          /* 负数表示底层 I2C 错误。 */
		return ret;                                     /* 保留底层错误码。 */

	return -EIO;                                          /* 非负但不是 1，表示写传输不完整。 */
}

/*
 * goodix_limit_coord - 把触摸 IC 原始坐标限制在 input 坐标范围内。
 * @value: Goodix 寄存器解析出的原始坐标。
 * @max: 当前轴最大分辨率，例如 1024 或 600。
 *
 * 返回值：
 * 合法范围内的坐标，范围是 0 到 max - 1。
 */
static u16 goodix_limit_coord(u16 value, u32 max)
{
	if (!max)                                            /* 理论上 probe 已经防止 max 为 0，这里保留保护。 */
		return 0;                                      /* max 为 0 时只能返回 0，避免 max - 1 下溢。 */

	if (value >= max)                                    /* 如果触摸 IC 返回了超出屏幕范围的坐标。 */
		return (u16)(max - 1);                          /* 裁剪到该轴最大合法坐标。 */

	return value;                                        /* 坐标本身合法，直接返回。 */
}

/*
 * goodix_release_all_slots - 释放当前驱动记录为按下状态的所有触摸 slot。
 * @ts: 驱动私有数据，里面保存 input 设备和 slot 状态。
 *
 * 使用场景：
 * 1. I2C 读取失败时，防止用户空间看到触摸点一直按下。
 * 2. Goodix 返回异常触点数量时，清理旧状态。
 */
static void goodix_release_all_slots(struct goodix_ts_data *ts)
{
	struct input_dev *input = ts->input;                 /* 取出 input 设备，用于上报释放事件。 */
	bool changed = false;                                /* 标记本次是否真的释放了 slot。 */
	int i;                                               /* 循环变量，用于遍历所有 slot。 */

	if (!input)                                          /* input 还没有注册时不能上报事件。 */
		return;                                        /* 直接返回，避免空指针访问。 */

	for (i = 0; i < GOODIX_MAX_POINTS; i++) {            /* 遍历所有触摸 slot。 */
		if (!ts->slot_active[i])                         /* 如果该 slot 本来就是松开状态。 */
			continue;                                  /* 不需要重复上报释放。 */

		input_mt_slot(input, i);                         /* 切换到需要释放的 slot。 */
		input_mt_report_slot_state(input, MT_TOOL_FINGER, false); /* 上报该 slot 手指已经离开。 */
		ts->slot_active[i] = false;                      /* 更新软件状态，记录该 slot 已释放。 */
		changed = true;                                  /* 标记本次产生了释放事件。 */
	}

	if (!changed)                                        /* 如果没有任何 slot 变化。 */
		return;                                        /* 不提交空事件帧。 */

	input_report_key(input, BTN_TOUCH, 0);                /* 上报整体触摸状态为松开。 */
	input_mt_sync_frame(input);                           /* 同步本帧多点触摸状态。 */
	input_sync(input);                                    /* 提交事件帧，让用户空间立即看到释放。 */
}

/*
 * goodix_report_events - 解析一帧 Goodix 触摸数据并上报 input 事件。
 * @ts: 驱动私有数据，包含 input 设备、坐标范围和 slot 状态。
 * @buf: 从 GOODIX_STATUS_REG 开始读出的原始数据，buf[0] 是状态寄存器。
 *
 * Goodix 数据格式：
 * buf[0] bit7 表示有新数据，bit3:0 表示触点数量。
 * 每个触点 8 字节，第 1 字节是触点 ID，第 2/3 字节是 X，第 4/5 字节是 Y。
 */
static void goodix_report_events(struct goodix_ts_data *ts, u8 *buf)
{
	struct input_dev *input = ts->input;                 /* 取出 input 设备，后续坐标都通过它上报。 */
	bool seen[GOODIX_MAX_POINTS] = { false };            /* 标记本帧中哪些 slot 仍然存在。 */
	bool touch_active = false;                           /* 标记本帧是否存在至少一个有效触点。 */
	bool primary_reported = false;                       /* 标记是否已经上报传统单点 ABS_X/ABS_Y。 */
	u8 status = buf[0];                                  /* 保存 Goodix 状态寄存器原始值。 */
	u8 touch_num = status & 0x0f;                         /* 状态寄存器低 4 位是触点数量。 */
	int i;                                               /* 循环变量，用于遍历触点和 slot。 */

	if (!(status & GOODIX_STATUS_READY))                  /* bit7 未置位表示当前没有新触摸数据。 */
		return;                                        /* 直接返回，避免重复处理旧帧。 */

	if (touch_num > GOODIX_MAX_POINTS) {                  /* 如果硬件返回的触点数量超过驱动支持范围。 */
		dev_warn_ratelimited(&ts->client->dev,           /* 限速打印警告，避免异常中断刷屏。 */
				     "invalid touch count %u\n", touch_num); /* 打印异常触点数量。 */
		goodix_release_all_slots(ts);                    /* 释放旧触点，避免状态卡住。 */
		return;                                        /* 异常帧不继续解析。 */
	}

	for (i = 0; i < touch_num; i++) {                     /* 遍历当前帧中的每一个触点。 */
		u8 *point = &buf[1 + i * GOODIX_POINT_SIZE];     /* 指向当前触点的 8 字节原始数据。 */
		u8 touch_id = point[0] & 0x0f;                   /* 触点 ID 位于第 0 字节低 4 位。 */
		int slot = touch_id < GOODIX_MAX_POINTS ? touch_id : i; /* 优先用硬件 ID，异常 ID 回退到顺序号。 */
		u16 raw_x = point[1] | (point[2] << 8);          /* X 坐标是小端格式：低字节在前，高字节在后。 */
		u16 raw_y = point[3] | (point[4] << 8);          /* Y 坐标同样是小端格式。 */
		u16 x = goodix_limit_coord(raw_x, ts->max_x);    /* 把 X 坐标限制到 input 声明范围。 */
		u16 y = goodix_limit_coord(raw_y, ts->max_y);    /* 把 Y 坐标限制到 input 声明范围。 */

		seen[slot] = true;                              /* 标记当前 slot 本帧仍然存在。 */
		touch_active = true;                            /* 标记当前帧存在有效触摸。 */
		ts->slot_active[slot] = true;                   /* 更新软件状态，记录该 slot 正在按下。 */

		input_mt_slot(input, slot);                     /* 切换到当前触点对应的 slot。 */
		input_mt_report_slot_state(input, MT_TOOL_FINGER, true); /* 上报当前 slot 有手指接触。 */
		input_report_abs(input, ABS_MT_POSITION_X, x);  /* 上报当前 slot 的 X 坐标。 */
		input_report_abs(input, ABS_MT_POSITION_Y, y);  /* 上报当前 slot 的 Y 坐标。 */

		if (!primary_reported) {                        /* 只把第一个有效触点同步到传统单点事件。 */
			input_report_abs(input, ABS_X, x);          /* 上报传统单点 X 坐标。 */
			input_report_abs(input, ABS_Y, y);          /* 上报传统单点 Y 坐标。 */
			primary_reported = true;                    /* 标记传统单点坐标已经上报。 */
		}
	}

	for (i = 0; i < GOODIX_MAX_POINTS; i++) {             /* 检查上一帧按下但本帧消失的 slot。 */
		if (seen[i] || !ts->slot_active[i])               /* 本帧仍存在，或者原本就没有按下。 */
			continue;                                  /* 不需要释放该 slot。 */

		input_mt_slot(input, i);                          /* 切换到需要释放的 slot。 */
		input_mt_report_slot_state(input, MT_TOOL_FINGER, false); /* 上报该 slot 已经抬起。 */
		ts->slot_active[i] = false;                       /* 更新软件状态为已释放。 */
	}

	input_report_key(input, BTN_TOUCH, touch_active);       /* 有触点则 BTN_TOUCH=1，没有触点则 BTN_TOUCH=0。 */
	input_mt_sync_frame(input);                             /* 同步本帧所有多点触摸 slot 状态。 */
	input_sync(input);                                      /* 提交本帧事件，让用户空间读取。 */
}

/*
 * goodix_irq_thread - Goodix 触摸中断线程。
 * @irq: 当前 IRQ 号，本函数不直接使用。
 * @dev_id: 申请中断时传入的 goodix_ts_data 指针。
 *
 * 这个函数运行在线程化中断上下文，可以安全调用 I2C 读写接口。
 */
static irqreturn_t goodix_irq_thread(int irq, void *dev_id)
{
	struct goodix_ts_data *ts = dev_id;                  /* 把 dev_id 转回驱动私有数据。 */
	u8 buf[GOODIX_DATA_LEN];                             /* 保存状态寄存器和最多 5 个触点的原始数据。 */
	u8 clear = 0;                                        /* 写 0 到状态寄存器，用于通知 Goodix 本帧已处理。 */
	int ret;                                             /* 保存 I2C 读写返回值。 */

	ret = goodix_read_regs(ts, GOODIX_STATUS_REG, buf, sizeof(buf)); /* 一次读取状态和触点数据。 */
	if (ret) {                                           /* 如果 I2C 读取失败。 */
		dev_err_ratelimited(&ts->client->dev,            /* 限速打印错误，避免中断风暴刷屏。 */
				    "read touch data failed: %d\n", ret); /* 打印 I2C 错误码。 */
		goodix_release_all_slots(ts);                    /* 释放旧触点，避免应用层触摸卡住。 */
		return IRQ_HANDLED;                              /* 中断已经处理，返回 HANDLED。 */
	}

	goodix_report_events(ts, buf);                        /* 解析触摸数据并上报 input 事件。 */

	if (buf[0] & GOODIX_STATUS_READY) {                    /* 只有处理了新数据时才清状态寄存器。 */
		ret = goodix_write_regs(ts, GOODIX_STATUS_REG, &clear, 1); /* 写 0 清除 Goodix 数据就绪标志。 */
		if (ret)                                         /* 如果清除状态失败，下一次可能重复收到同一帧。 */
			dev_err_ratelimited(&ts->client->dev,        /* 限速打印状态清除失败。 */
					    "clear touch status failed: %d\n", ret); /* 打印 I2C 错误码。 */
	}

	return IRQ_HANDLED;                                   /* 返回 IRQ_HANDLED，表示中断线程处理完成。 */
}

/*
 * goodix_hw_reset - 使用 reset-gpios 和 irq-gpios 执行 Goodix 地址选择和硬件复位。
 * @ts: 驱动私有数据，里面保存 I2C client 和 GPIO 编号。
 *
 * 地址选择说明：
 * 当前板子通过 i2cdetect 已确认 Goodix 芯片在 7-bit 地址 0x5d。
 * Goodix 上电/复位时会采样 INT 引脚状态选择地址：
 * INT 高电平选择 0x14，INT 低电平选择 0x5d。
 * 因此这里在释放 RESET 前保持 INT 为低电平。
 *
 * 返回值：
 * 0 表示复位成功；负数表示 GPIO 申请或方向配置失败。
 */
static int goodix_hw_reset(struct goodix_ts_data *ts)
{
	struct device *dev = &ts->client->dev;               /* 取出设备对象，用于 devm GPIO 资源管理。 */
	int ret;                                             /* 保存 GPIO 申请和方向配置返回值。 */

	ts->reset_gpio = of_get_named_gpio(dev->of_node, "reset-gpios", 0); /* 从设备树读取复位 GPIO。 */
	if (ts->reset_gpio == -EPROBE_DEFER)                 /* GPIO 控制器尚未准备好时需要延迟 probe。 */
		return ts->reset_gpio;                          /* 返回 -EPROBE_DEFER，让内核稍后重试。 */
	if (!gpio_is_valid(ts->reset_gpio)) {                /* 如果设备树没有有效 reset-gpios。 */
		dev_err(dev, "missing reset-gpios\n");           /* 打印错误，Goodix 地址选择和复位依赖 reset 脚。 */
		return -EINVAL;                                  /* 返回参数错误，中止 probe。 */
	}

	ts->irq_gpio = of_get_named_gpio(dev->of_node, "irq-gpios", 0); /* 优先读取当前设备树使用的 irq-gpios。 */
	if (ts->irq_gpio == -ENOENT || ts->irq_gpio == -EINVAL) /* 如果没有 irq-gpios。 */
		ts->irq_gpio = of_get_named_gpio(dev->of_node, "interrupt-gpios", 0); /* 兼容官方例程的 interrupt-gpios。 */
	if (ts->irq_gpio == -EPROBE_DEFER)                   /* GPIO 控制器尚未准备好时需要延迟 probe。 */
		return ts->irq_gpio;                            /* 返回 -EPROBE_DEFER，让内核稍后重试。 */
	if (!gpio_is_valid(ts->irq_gpio)) {                  /* 如果设备树没有有效中断 GPIO。 */
		dev_err(dev, "missing irq-gpios or interrupt-gpios\n"); /* 打印错误，触摸驱动不能用轮询替代。 */
		return -EINVAL;                                  /* 返回参数错误，中止 probe。 */
	}

	ret = devm_gpio_request_one(dev, ts->reset_gpio, GPIOF_OUT_INIT_HIGH, "gt9147 reset"); /* 申请复位 GPIO，初始为释放复位。 */
	if (ret) {                                            /* 如果复位 GPIO 申请失败。 */
		dev_err(dev, "request reset gpio %d failed: %d\n", ts->reset_gpio, ret); /* 打印失败 GPIO 和错误码。 */
		return ret;                                      /* 返回错误码，中止 probe。 */
	}

	ret = devm_gpio_request_one(dev, ts->irq_gpio, GPIOF_OUT_INIT_LOW, "gt9147 irq"); /* 申请 INT GPIO，先输出低电平选择 0x5d。 */
	if (ret) {                                            /* 如果中断 GPIO 申请失败。 */
		dev_err(dev, "request irq gpio %d failed: %d\n", ts->irq_gpio, ret); /* 打印失败 GPIO 和错误码。 */
		return ret;                                      /* 返回错误码，中止 probe。 */
	}

	gpio_set_value_cansleep(ts->reset_gpio, 0);            /* 拉低 RESET，让 Goodix 进入硬件复位。 */
	msleep(GOODIX_RESET_LOW_MS);                           /* 保持复位一段时间，保证芯片真正复位。 */
	gpio_set_value_cansleep(ts->irq_gpio, 0);              /* 复位释放前保持 INT 低电平，用于选择当前硬件扫到的 0x5d 地址。 */
	gpio_set_value_cansleep(ts->reset_gpio, 1);            /* 拉高 RESET，释放硬件复位。 */
	msleep(GOODIX_RESET_HIGH_MS);                          /* 等待芯片内部固件启动完成。 */
	gpio_set_value_cansleep(ts->irq_gpio, 0);              /* 复位完成后短暂拉低 INT，贴近官方 GT9147 例程时序。 */
	msleep(GOODIX_INT_READY_MS);                           /* 等待 INT 状态稳定。 */
	ret = gpio_direction_input(ts->irq_gpio);              /* 把 INT 切换为输入，后续由触摸 IC 驱动中断电平。 */
	if (ret) {                                            /* 如果 GPIO 方向切换失败。 */
		dev_err(dev, "set irq gpio input failed: %d\n", ret); /* 打印方向切换失败错误码。 */
		return ret;                                      /* 返回错误码，中止 probe。 */
	}

	return 0;                                             /* Goodix 复位和地址选择流程完成。 */
}

/*
 * goodix_request_irq - 申请 Goodix 线程化中断。
 * @ts: 驱动私有数据，函数会写入 ts->irq。
 *
 * 返回值：
 * 0 表示中断申请成功；负数表示失败。
 */
static int goodix_request_irq(struct goodix_ts_data *ts)
{
	struct device *dev = &ts->client->dev;               /* 取出设备对象，用于申请 devm IRQ。 */
	int ret;                                             /* 保存 gpio_to_irq() 或 request_irq() 返回值。 */

	ts->irq = ts->client->irq;                            /* 先使用标准 interrupts 属性解析出的 IRQ。 */
	if (ts->irq <= 0 && gpio_is_valid(ts->irq_gpio)) {    /* 如果设备树只有 irq-gpios，没有标准 interrupts。 */
		ret = gpio_to_irq(ts->irq_gpio);                 /* 把 GPIO 编号转换为 Linux IRQ 编号。 */
		if (ret < 0) {                                   /* 如果 GPIO 控制器不能映射 IRQ。 */
			dev_err(dev, "gpio_to_irq(%d) failed: %d\n", ts->irq_gpio, ret); /* 打印失败原因。 */
			return ret;                                  /* 返回错误码，中止 probe。 */
		}
		ts->irq = ret;                                   /* 保存 gpio_to_irq() 得到的 IRQ 编号。 */
	}

	if (ts->irq <= 0) {                                   /* 如果仍然没有有效 IRQ。 */
		dev_err(dev, "missing touchscreen irq\n");        /* 打印错误，提示设备树缺中断配置。 */
		return -EINVAL;                                  /* 返回参数错误，中止 probe。 */
	}

	ret = devm_request_threaded_irq(dev, ts->irq, NULL,   /* 申请线程化中断，硬中断处理函数为 NULL。 */
					goodix_irq_thread, IRQF_TRIGGER_FALLING | IRQF_ONESHOT, /* 下降沿触发，ONESHOT 防止线程重入。 */
					dev_name(dev), ts);                 /* 中断名用设备名，dev_id 传入驱动私有数据。 */
	if (ret) {                                            /* 如果中断申请失败。 */
		dev_err(dev, "request threaded irq %d failed: %d\n", ts->irq, ret); /* 打印 IRQ 和错误码。 */
		return ret;                                      /* 返回错误码，中止 probe。 */
	}

	return 0;                                             /* 中断申请成功。 */
}

/*
 * goodix_input_init - 初始化并注册 Goodix input 设备。
 * @ts: 驱动私有数据，函数会设置 ts->input。
 *
 * 返回值：
 * 0 表示 input 设备注册成功；负数表示失败。
 */
static int goodix_input_init(struct goodix_ts_data *ts)
{
	struct device *dev = &ts->client->dev;                /* 取出设备对象，用于 devm 分配 input 设备。 */
	struct input_dev *input;                              /* 保存即将分配的 input 设备指针。 */
	int ret;                                              /* 保存多点 slot 初始化或 input 注册返回值。 */

	input = devm_input_allocate_device(dev);               /* 分配由 devm 管理的 input_dev。 */
	if (!input)                                           /* 如果 input_dev 分配失败，说明内存不足。 */
		return -ENOMEM;                                  /* 返回内存不足错误码。 */

	input->name = "Goodix GT911/GT9147 Capacitive Touch";  /* 设置 input 设备名称，便于 /proc/bus/input/devices 识别。 */
	input->id.bustype = BUS_I2C;                           /* 标记该 input 设备来自 I2C 总线。 */
	input_set_drvdata(input, ts);                          /* 把驱动私有数据挂到 input_dev 上，便于后续扩展。 */

	__set_bit(EV_KEY, input->evbit);                       /* 声明支持按键类事件，用于 BTN_TOUCH。 */
	__set_bit(BTN_TOUCH, input->keybit);                   /* 声明支持整体触摸按下/抬起状态。 */

	input_set_abs_params(input, ABS_X, 0, ts->max_x - 1, 0, 0); /* 设置传统单点 X 坐标范围。 */
	input_set_abs_params(input, ABS_Y, 0, ts->max_y - 1, 0, 0); /* 设置传统单点 Y 坐标范围。 */
	input_set_abs_params(input, ABS_MT_POSITION_X, 0, ts->max_x - 1, 0, 0); /* 设置多点触摸 X 坐标范围。 */
	input_set_abs_params(input, ABS_MT_POSITION_Y, 0, ts->max_y - 1, 0, 0); /* 设置多点触摸 Y 坐标范围。 */

	ret = input_mt_init_slots(input, GOODIX_MAX_POINTS, INPUT_MT_DIRECT); /* 初始化直接触摸类型的多点 slot。 */
	if (ret)                                              /* 如果 slot 初始化失败。 */
		return ret;                                      /* 返回错误码，中止 input 初始化。 */

	ts->input = input;                                     /* 保存 input_dev，供中断线程上报事件。 */
	return input_register_device(input);                    /* 注册 input 设备，成功后生成 /dev/input/eventX。 */
}

/*
 * goodix_identify - 读取并打印 Goodix 产品 ID 和配置版本。
 * @ts: 驱动私有数据，函数通过 I2C 读取 GOODIX_PID_REG。
 *
 * 返回值：
 * 0 表示读取成功；负数表示 I2C 通信失败。
 */
static int goodix_identify(struct goodix_ts_data *ts)
{
	u8 pid[6];                                           /* 保存从 0x8140 读取的产品 ID 和版本信息。 */
	char product[5];                                     /* 保存可打印的产品 ID 字符串。 */
	int ret;                                             /* 保存 I2C 读取返回值。 */

	ret = goodix_read_regs(ts, GOODIX_PID_REG, pid, sizeof(pid)); /* 读取产品 ID，例如 GT911 返回 "911"。 */
	if (ret)                                             /* 如果 I2C 读取失败，说明地址或复位仍有问题。 */
		return ret;                                     /* 返回错误码，由 probe 打印并中止。 */

	memcpy(product, pid, 4);                              /* 复制最多 4 个产品 ID 字节。 */
	product[4] = '\0';                                    /* 手动补字符串结束符，避免日志越界。 */

	dev_info(&ts->client->dev, "Goodix product id '%s', cfg version 0x%02x, sensor id 0x%02x\n",
		 product, pid[4], pid[5]);                      /* 打印产品 ID、配置版本和传感器 ID，便于确认芯片。 */
	return 0;                                             /* 产品 ID 读取成功。 */
}

/*
 * goodix_probe - Goodix I2C 驱动探测函数。
 * @client: 内核根据设备树 I2C 节点创建的设备对象。
 * @id: I2C ID 表匹配项，设备树匹配时通常不使用。
 *
 * 主要流程：
 * 1. 检查 I2C 控制器能力。
 * 2. 分配私有数据并读取屏幕分辨率。
 * 3. 使用 RESET/INT GPIO 选择地址并复位 Goodix。
 * 4. 读取产品 ID，确认 I2C 通信正常。
 * 5. 注册 input 设备。
 * 6. 申请线程化中断。
 */
static int goodix_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct goodix_ts_data *ts;                            /* 保存即将分配的 Goodix 私有数据。 */
	int ret;                                              /* 保存各初始化步骤返回值。 */

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) /* 确认当前 I2C 控制器支持基础 I2C 传输。 */
		return -EOPNOTSUPP;                             /* 控制器不支持时驱动无法工作。 */

	ts = devm_kzalloc(&client->dev, sizeof(*ts), GFP_KERNEL); /* 分配并清零驱动私有数据。 */
	if (!ts)                                              /* 如果内存分配失败。 */
		return -ENOMEM;                                  /* 返回内存不足错误。 */

	ts->client = client;                                  /* 保存 I2C client，后续所有寄存器访问都需要它。 */
	ts->max_x = GOODIX_DEFAULT_MAX_X;                     /* 设置默认 X 轴范围为 1024。 */
	ts->max_y = GOODIX_DEFAULT_MAX_Y;                     /* 设置默认 Y 轴范围为 600。 */
	ts->reset_gpio = -EINVAL;                             /* 初始化复位 GPIO 为无效状态。 */
	ts->irq_gpio = -EINVAL;                               /* 初始化中断 GPIO 为无效状态。 */
	ts->irq = -EINVAL;                                    /* 初始化 IRQ 为无效状态。 */
	i2c_set_clientdata(client, ts);                        /* 把私有数据保存到 client，便于 remove 或调试取回。 */

	of_property_read_u32(client->dev.of_node, "touchscreen-size-x", &ts->max_x); /* 从设备树读取 X 分辨率。 */
	of_property_read_u32(client->dev.of_node, "touchscreen-size-y", &ts->max_y); /* 从设备树读取 Y 分辨率。 */
	if (!ts->max_x) {                                     /* 如果设备树把 X 分辨率错误写成 0。 */
		dev_warn(&client->dev, "touchscreen-size-x is 0, fallback to %u\n", GOODIX_DEFAULT_MAX_X); /* 打印回退提示。 */
		ts->max_x = GOODIX_DEFAULT_MAX_X;                /* 回退默认 X 分辨率，避免 max_x - 1 下溢。 */
	}
	if (!ts->max_y) {                                     /* 如果设备树把 Y 分辨率错误写成 0。 */
		dev_warn(&client->dev, "touchscreen-size-y is 0, fallback to %u\n", GOODIX_DEFAULT_MAX_Y); /* 打印回退提示。 */
		ts->max_y = GOODIX_DEFAULT_MAX_Y;                /* 回退默认 Y 分辨率，避免 max_y - 1 下溢。 */
	}

	ret = goodix_hw_reset(ts);                            /* 执行 Goodix 硬件复位和 I2C 地址选择。 */
	if (ret) {                                            /* 如果复位或 GPIO 申请失败。 */
		dev_err(&client->dev, "hardware reset failed: %d\n", ret); /* 打印复位失败错误码。 */
		return ret;                                      /* 返回错误码，中止 probe。 */
	}

	ret = goodix_identify(ts);                            /* 读取产品 ID，确认 0x5d 地址上确实是 Goodix 芯片。 */
	if (ret) {                                            /* 如果产品 ID 读取失败。 */
		dev_err(&client->dev, "read product id failed: %d\n", ret); /* 打印 I2C 通信失败错误码。 */
		return ret;                                      /* 返回错误码，中止 probe。 */
	}

	ret = goodix_input_init(ts);                           /* 初始化并注册 input 设备。 */
	if (ret) {                                            /* 如果 input 注册失败。 */
		dev_err(&client->dev, "register input device failed: %d\n", ret); /* 打印 input 初始化错误。 */
		return ret;                                      /* 返回错误码，中止 probe。 */
	}

	ret = goodix_request_irq(ts);                          /* 申请线程化触摸中断。 */
	if (ret) {                                            /* 如果中断申请失败。 */
		dev_err(&client->dev, "request irq failed: %d\n", ret); /* 打印中断申请错误码。 */
		return ret;                                      /* 返回错误码，中止 probe。 */
	}

	dev_info(&client->dev, "Goodix touch initialized, addr 0x%02x, max %ux%u, irq %d, reset gpio %d, irq gpio %d\n",
		 client->addr, ts->max_x, ts->max_y, ts->irq, ts->reset_gpio, ts->irq_gpio); /* 打印初始化成功信息。 */
	return 0;                                             /* 返回 0，表示驱动绑定成功。 */
}

/*
 * goodix_remove - Goodix I2C 设备移除函数。
 * @client: 即将解绑的 I2C 设备。
 *
 * 本驱动使用 devm_* 管理 GPIO、IRQ 和 input 资源。
 * 模块卸载或设备解绑时资源会自动释放，因此这里不需要手工清理。
 */
static int goodix_remove(struct i2c_client *client)
{
	return 0;                                             /* 没有额外手工资源需要释放，直接返回成功。 */
}

static const struct of_device_id goodix_of_match[] = {       /* 定义设备树匹配表，用于通过 compatible 自动绑定驱动。 */
	{ .compatible = "goodix,gt911" },                    /* 匹配当前板子实际读到的 GT911/GT9xx 产品 ID。 */
	{ .compatible = "goodix,gt9147" },                   /* 兼容课程和官方例程常用的 GT9147 命名。 */
	{ .compatible = "atk-gt9147" },                      /* 兼容正点原子官方 gt9147.c 的 compatible。 */
	{ }                                                   /* 空元素表示匹配表结束。 */
};
MODULE_DEVICE_TABLE(of, goodix_of_match);                    /* 导出 OF 匹配表，支持模块自动加载。 */

static const struct i2c_device_id goodix_id[] = {             /* 定义传统 I2C ID 表，用于非设备树实例化。 */
	{ "gt911", 0 },                                       /* 匹配 GT911 设备名。 */
	{ "gt9147", 0 },                                      /* 匹配 GT9147 设备名。 */
	{ "atk-gt9147", 0 },                                  /* 匹配正点原子官方设备名。 */
	{ }                                                   /* 空元素表示 ID 表结束。 */
};
MODULE_DEVICE_TABLE(i2c, goodix_id);                         /* 导出 I2C ID 表，支持 modinfo 查询。 */

static struct i2c_driver goodix_driver = {                    /* 定义 Goodix I2C 驱动主体。 */
	.probe = goodix_probe,                                  /* I2C 设备匹配成功后调用 goodix_probe()。 */
	.remove = goodix_remove,                                /* 设备解绑或模块卸载时调用 goodix_remove()。 */
	.id_table = goodix_id,                                  /* 指定传统 I2C ID 匹配表。 */
	.driver = {                                             /* 内嵌通用设备驱动信息。 */
		.name = "atk-gt9147",                              /* 驱动名称，保留课程例程风格，日志中也会显示它。 */
		.of_match_table = goodix_of_match,                 /* 指定设备树匹配表。 */
	},                                                      /* 通用设备驱动信息结束。 */
};

module_i2c_driver(goodix_driver);                            /* 自动生成模块加载/卸载入口并注册 I2C 驱动。 */

MODULE_LICENSE("GPL");                                       /* 声明模块许可证为 GPL。 */
MODULE_AUTHOR("CFR");                                        /* 声明当前适配版本作者。 */
MODULE_DESCRIPTION("Goodix GT911/GT9147 I2C touchscreen driver with threaded IRQ"); /* 声明模块功能描述。 */
