# 15_iictouch Goodix I2C 触摸屏模块说明

## 模块目的

| 项目 | 内容 |
|---|---|
| 模块目的 | 适配 Goodix GT911/GT9147 电容触摸屏，通过 I2C 读取触摸点并上报 Linux input 多点触摸事件。 |
| 内核模块 | `iictouch.ko`。 |
| 用户程序 | `iictouchapp`，读取 `/dev/input/eventX` 并打印 slot、坐标和 BTN_TOUCH 状态。 |
| input 名称 | `Goodix GT911/GT9147 Capacitive Touch`。 |
| 支持 compatible | `"goodix,gt911"`、`"goodix,gt9147"`、`"atk-gt9147"`。 |
| 默认分辨率 | 1024x600，可由 `touchscreen-size-x/y` 覆盖。 |

## 修改文件清单

| 路径 | 作用与维护原因 |
|---|---|
| `15_iictouch/iictouch.c` | Goodix I2C 触摸驱动，完成复位、产品 ID 读取、线程化 IRQ、input 多点触摸上报。 |
| `15_iictouch/iictouchapp.c` | 用户态 input event 测试程序，打印触摸 slot 坐标和按下/抬起状态。 |
| `15_iictouch/Makefile` | 同时编译 `iictouch.ko` 和 `iictouchapp`。 |
| `15_iictouch/README.md` | 本模块说明文档，记录设备树契约、测试矩阵、事件验证和排障。 |

## 运行中恢复策略

| 项目 | 当前实现 |
|---|---|
| 触发条件 | `GOODIX_STATUS_REG` 读失败或清状态寄存器写失败，失败达到 `GOODIX_RECOVERY_FAIL_THRESHOLD = 1`。 |
| 典型错误 | `read touch data failed: -6`，其中 `-6` 是 `-ENXIO`，通常表示 I2C 地址无应答。 |
| 防刷屏/防抖 | 两次恢复之间至少间隔 `GOODIX_RECOVERY_COOLDOWN_MS = 3000 ms`，因此即使每次中断都读失败，也最多约 3 秒复位一次。 |
| 恢复上下文 | 直接在线程化 IRQ 中执行恢复；该 IRQ 使用 `IRQF_ONESHOT`，可以睡眠且能避免同一 IRQ 重入。 |
| 恢复动作 | 释放所有 active slot、执行 RESET/INT 复位时序、自动探测 `0x5d/0x14` 当前有效地址、重新读取产品 ID。 |
| 恢复范围 | 不重新注册 input 设备，不重新申请 GPIO/IRQ，只恢复当前 Goodix 控制器通信状态。 |
| 卸载保护 | `remove()` 设置 `stopping` 标志、释放 IRQ，并释放所有 slot。 |

## I2C 地址自动选择

| 项目 | 当前实现 |
|---|---|
| 设备树默认地址 | 节点仍然是 `gt9147@5d`，驱动 probe 时优先尝试 `0x5d`。 |
| 备用地址 | 如果 `0x5d` 读取 PID 失败，驱动会自动尝试 Goodix 常见备用地址 `0x14`。 |
| 触发时机 | probe 初始复位后探测一次；运行中任意寄存器读写失败会立即重新探测；恢复复位后也会重新探测。 |
| 运行日志 | 如果切换地址，会看到 `Goodix addr 0x5d no ACK... try fallback addr 0x14` 和 `Goodix active I2C addr switched to 0x14`。 |
| 解决的问题 | 开发板复位后触摸 IC 可能采样 INT 引脚进入 `0x14`，而设备树仍绑定 `1-005d`，导致固定访问 `0x5d` 时持续 `-ENXIO`。 |

## 设备树契约

示例节点需放在实际 I2C 控制器下：

```dts
gt911@5d {
    compatible = "goodix,gt911";
    reg = <0x5d>;
    reset-gpios = <&gpiox n GPIO_ACTIVE_HIGH>;
    irq-gpios = <&gpiox n GPIO_ACTIVE_HIGH>;
    touchscreen-size-x = <1024>;
    touchscreen-size-y = <600>;
    status = "okay";
};
```

| 属性 | 要求 | 说明 |
|---|---|---|
| `compatible` | 三个支持字符串之一 | 用于 I2C 驱动匹配。 |
| `reg` | 常见 `0x5d` | 当前驱动优先按设备树 `0x5d` 处理；如果复位后 IC 实际落到 `0x14`，会自动切到 `0x14` 读写寄存器。 |
| `reset-gpios` | 必填 | 用于硬件复位和地址选择。 |
| `irq-gpios` 或 `interrupt-gpios` | 必填 | 用于触摸中断，驱动不走轮询替代。 |
| `touchscreen-size-x/y` | 可选 | 缺省为 1024x600；写 0 会回退默认值。 |

## 使用流程

| 阶段 | 执行位置 | 命令 | 预期结果 |
|---|---|---|---|
| 编译驱动和应用 | 虚拟机 `15_iictouch/` | `make` | 生成 `iictouch.ko` 和 `iictouchapp`。 |
| 部署文件 | 虚拟机 `15_iictouch/` | `cp iictouch.ko iictouchapp /home/cfr/linux/nfs/rootfs/root/` | 开发板 `/root/` 下可见。 |
| 确认 I2C 设备 | 开发板 | `i2cdetect -y <bus>` | 对应总线能看到 `0x5d` 或目标地址。 |
| 加载驱动 | 开发板 `/root/` | `insmod iictouch.ko` | `dmesg` 输出 Goodix product id 和 initialized 日志。 |
| 查找 event | 开发板 | `grep -A8 -B2 "Goodix GT911" /proc/bus/input/devices` | 找到对应 `eventX`。 |
| 运行触摸测试 | 开发板 `/root/` | `./iictouchapp /dev/input/eventX` | 手指触摸 LCD 时打印 slot 坐标和 BTN_TOUCH down/up。 |
| 查看恢复日志 | 开发板 | `dmesg -w | grep -Ei "atk-gt9147|touch recovery|read touch data failed"` | I2C 连续失败后能看到 `schedule touch recovery`、`start touch recovery`、成功时看到 `touch recovery completed`。 |
| 卸载驱动 | 开发板 `/root/` | `rmmod iictouch` | input 设备注销。 |

## 测试与验证矩阵

| Test goal | Run location | Command | Expected result | Failure triage |
|---|---|---|---|---|
| 编译模块和应用 | 虚拟机 `15_iictouch/` | `make` | `iictouch.ko`、`iictouchapp` 生成。 | 查 `CROSS_COMPILE` 是否匹配当前工具链；默认是 `arm-none-linux-gnueabihf-`。 |
| I2C 地址确认 | 开发板 | `i2cdetect -y <bus>` | 能看到 Goodix 地址，常见 `0x5d` 或 `0x14`；驱动绑定的地址会显示 `UU`。 | 若两个地址都看不到，先查触摸供电、I2C pinctrl、reset/irq 引脚和总线号。 |
| 驱动 probe | 开发板 `/root/` | `insmod iictouch.ko; dmesg | tail -n 100` | 输出 `Goodix product id ...` 和 `Goodix touch initialized ...`。 | 若 `missing reset-gpios` 或 `missing irq-gpios`，修 DTS；若 product id 读取失败，查 I2C 地址和复位时序。 |
| input 设备发现 | 开发板 | `grep -A8 -B2 "Goodix GT911/GT9147 Capacitive Touch" /proc/bus/input/devices` | 能找到 `Handlers=... eventX`。 | 若 probe 成功但无 event，查 `input_register_device` 是否失败。 |
| 单点触摸 | 开发板 `/root/` | `./iictouchapp /dev/input/eventX` | 触摸时打印 `slot 0 down`、坐标和 `BTN_TOUCH down/up`。 | 若无输出，查 IRQ 是否触发：`cat /proc/interrupts | grep -i goodix`。 |
| 多点触摸 | 开发板 `/root/` | `./iictouchapp /dev/input/eventX` | 多指触摸时多个 slot 坐标同时更新。 | 若坐标异常，查 `touchscreen-size-x/y` 和屏幕方向映射。 |
| I2C 掉线自动恢复 | 开发板 | `cat /dev/kmsg | grep -Ei "read touch data failed|schedule touch recovery|start touch recovery|touch recovery completed|active I2C addr|touch recovery identify failed"` | I2C 失败后最多每 3 秒调度一次恢复；恢复成功后重新打印 Goodix product id、当前 active 地址和 `touch recovery completed`。 | 如果 `0x5d` 和 `0x14` 都失败，说明复位后芯片仍不应答，继续查供电、排线、I2C 上拉、RESET/INT 引脚和 I2C 总线占用。 |
| 通用工具交叉验证 | 开发板 | `evtest /dev/input/eventX` | 能看到 `EV_ABS`、`ABS_MT_POSITION_X/Y`、`BTN_TOUCH`。 | 若没有 `evtest`，使用 `iictouchapp`。 |
| 卸载清理 | 开发板 `/root/` | `rmmod iictouch` | event 节点消失。 | 若模块忙，先停止 `iictouchapp` 或 Qt 触摸程序。 |

## 读写验证与数据路径

| 路径 | 验证方式 | 说明 |
|---|---|---|
| I2C 读路径 | `dmesg | grep "Goodix product id"` | 驱动通过 I2C 读取 PID、配置版本和触点寄存器。 |
| input 事件读路径 | `./iictouchapp /dev/input/eventX` | 这是用户态验证触摸数据的主路径。 |
| 写路径 | 驱动内部写 `GOODIX_STATUS_REG = 0` 清中断状态 | 用户态不直接写触摸设备；清状态由驱动完成。 |
| 恢复写路径 | 驱动内部控制 `reset-gpios` 和 `irq-gpios` | 运行中 I2C 连续失败后，驱动会重新执行 Goodix 地址选择和硬件复位时序。 |
| 地址 fallback 路径 | 驱动内部临时访问 `0x5d/0x14` | 读取 PID 成功的地址会保存为 `active_addr`，后续触点读取和状态清除都走该地址。 |

## 文件保存和图片等待规则

本触摸模块本身不保存图片，但它常和 Qt 摄像头界面一起使用。若触摸触发拍照并保存到 SD 卡，等待规则必须写在调用脚本或应用文档中：

当前触摸模块没有内置拍照命令；下面片段用于等待“触摸触发的实际拍照程序已经开始写入的图片文件”完成：

```sh
mkdir -p /mnt/sdcard/images
file=/mnt/sdcard/images/touch_capture.jpg
# 先运行实际触摸拍照程序，让它把图片保存到 "$file"。
while true; do
    s1=$(stat -c %s "$file" 2>/dev/null || echo 0)
    sleep 1
    s2=$(stat -c %s "$file" 2>/dev/null || echo 0)
    [ "$s1" = "$s2" ] && [ "$s1" -gt 0 ] && break
done
sync
sdcard-safe-remove
```

关键点：等待拍照进程退出或明确完成事件，确认图片文件存在且大小稳定，再 `sync` 和安全卸载。不能只靠固定 `sleep` 判断图片已写完。

## 硬件资源

| 资源 | 用途 | 契约 |
|---|---|---|
| Goodix GT911/GT9147 | 触摸控制器 | I2C 通信正常后才能读取 PID 和触点数据。 |
| I2C 总线 | 控制器通信 | DTS 节点必须挂在正确 I2C 控制器下。 |
| `reset-gpios` | 地址选择和硬件复位 | 缺失时驱动直接 probe 失败。 |
| `irq-gpios`/`interrupt-gpios` | 触摸中断 | 缺失时不允许降级轮询。 |
| `/dev/input/eventX` | 用户态事件接口 | event 编号会变化，必须动态查询。 |

## 修改记录

| 时间 | 修改点 | 结果 |
|---|---|---|
| 2026-05-20 | 为寄存器读写增加失败后立即重新探测 `0x5d/0x14` 并重试 | 触摸 IC 已经跑到备用地址时，不必等完整复位流程，驱动会先切换 active 地址继续读写。 |
| 2026-05-20 | 将恢复流程从 workqueue 改为 threaded IRQ 内同步执行 | 避免恢复 work 调用 `disable_irq()` 等待当前 IRQ 线程造成恢复 pending 卡住，导致只刷 `read touch data failed`。 |
| 2026-05-20 | 增加 Goodix `0x5d/0x14` 地址自动选择 | 解决开发板复位后触摸 IC 可能落到 `0x14`，而设备树固定 `0x5d` 导致一直 `-ENXIO` 的问题。 |
| 2026-05-19 | 为 `iictouch.c` 增加连续 I2C 失败后的异步恢复逻辑 | `read touch data failed: -6` 连续出现时，驱动会限频复位触摸 IC、重新读取 PID，并释放旧 slot 防止 UI 卡住。 |
| 2026-05-03 | 新增模块 README | 按新版提示词补齐设备树契约、测试矩阵、触摸事件验证和图片保存等待规则。 |
