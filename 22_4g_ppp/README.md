# STM32MP157 4G PPP 拨号与 COS 网络模块文档

## 模块目的

| 项目 | 内容 |
|---|---|
| 模块目的 | 在 STM32MP157 开发板上通过 EC20/Quectel 4G 模块建立 PPP 网络，提供外网访问和后续腾讯 COS HTTPS 上传能力。 |
| 4G 模块 | EC20/Quectel 类 USB 4G modem。 |
| SIM 类型 | 当前测试路径为中国电信卡，APN 使用 `CTNET`。 |
| 拨号串口 | `/dev/ttyUSB2`。 |
| PPP 接口 | 成功后生成 `ppp0`。 |
| 管理命令 | `4g-ppp {start|stop|restart|status|test|sim-status|monitor-start|monitor-stop}`。 |
| 定位命令 | `4g-location {once|status|location-status}`，只调用高德 IP 定位接口，按 4G 出口公网 IP 获取省份；状态文件只显示省份，例如 `河南省`，不再打开 GPS，也不再访问 ttyUSB AT 串口。 |
| 校时命令 | `board-time-sync {once|status}`，联网后同步北京时间并写回 RTC。 |
| 登录时区 | `/etc/profile.d/board-timezone.sh`，让 root 登录后裸 `date` 默认显示北京时间。 |
| 开机脚本 | `/etc/init.d/S80ppp-4g {start|stop|restart|status|test}`。 |
| USB 上电保护 | `4g-ppp` 会把 `/sys/bus/usb/devices/2-1/power/control` 和 `/sys/bus/usb/devices/2-1.7/power/control` 写成 `on`，避免 4G 模块和父级 USB hub 被 runtime suspend。 |
| SIM 热插拔恢复 | 当前原理图里 U22 的 `USIM_PRESENT` 没有接到 Nano SIM 卡座检测脚，因此软件不能依赖硬件插卡中断实现“零轮询立即唤醒”。新版 `4g-ppp monitor-start` 采用“无卡低频轮询 + 串口 URC 被动监听 + 插回短时快速确认”：刚拔卡后每 5 秒关注一次，超过 `SIM_RECENT_NO_CARD_WINDOW_SECONDS` 默认 8 秒后按 `SIM_LONG_NO_CARD_INTERVAL` 默认 8 秒主动兜底查一次；如果串口收到 SIM 插入/READY URC，则立即进入 1 秒一次、最多 30 秒的快速确认，然后自动重拨。遇到 `+CME ERROR: 13` 这类 `sim_error` 时，脚本先用 `SIM_ERROR_FAST_WAIT_SECONDS` 默认 10 秒做轻量快速确认，仍不 READY 时才按 `SIM_CFUN_SIM_ERROR_COOLDOWN_SECONDS` 默认 30 秒执行 `AT+CFUN=0/1`；长期无卡仍使用 `SIM_CFUN_COOLDOWN_SECONDS` 默认 300 秒，避免没卡时反复复位模块。PPP 成功上线后会清理本轮恢复时间戳，让下一次拔插测试不被上一轮冷却卡住。 |
| 重要结论 | 2026-05-20 现场出现 `usb 2-1.7: USB disconnect`、`ttyUSB0~3 disconnected/attached` 反复枚举，导致 Qt 真实健康检测显示网络离线；把 4G 模块和父级 hub 的 `power/control` 置为 `on` 后，30 秒观察内 USB 断连计数没有增加，`4g-ppp test` 通过。后续用户补足整板供电后复测，触摸屏恢复可触摸，4G 模块也不再时不时断线，因此凌晨问题优先归因为供电裕量不足或 USB 负载导致的硬件不稳定。2026-06-08 根据 4G 模块原理图确认 `USIM_PRESENT` 没有接到卡座检测脚，热插恢复不能依赖硬件插拔事件，改为自适应低频监控和插回快速确认。当前 EC20F 拨号脚本不要发送 `ATP`，否则 chat 会在 `AT+CGDCONT` 前因 `ERROR` 退出。开机校时必须在 4G 联网成功后做一次，校准系统时间后用 `hwclock -w -u` 写回 RTC。 |
| 开机兼容 | 原有 `/etc/init.d/S80ppp-4g start` 不需要改命令形状；新版 `4g-ppp start` 会先按旧流程尝试拨号，然后启动后台 monitor。即使开机时没有插 SIM，monitor 也会进入无卡低频等待，后续插回卡后自动快速确认和重拨。 |

## 修改文件清单

| 路径 | 修改原因 |
|---|---|
| 虚拟机内核仓库：`/home/cfr/linux/atk-mp1/linux/my_linux/linux-5.4.31/arch/arm/configs/stm32mp1_atk_defconfig` | 持久化 USB serial option、WWAN、PPP、PPP_ASYNC、PPP_SYNC_TTY、PPPOE、PPP 压缩/加密依赖等内核配置，确保干净构建仍支持 4G modem 和 PPP。 |
| 虚拟机内核仓库：`/home/cfr/linux/atk-mp1/linux/my_linux/linux-5.4.31/drivers/usb/serial/option.c` | 作为 USB option 驱动绑定 VID/PID 的源头，必要时添加 4G 模块 ID 和接口跳过规则。 |
| 虚拟机内核仓库：`/home/cfr/linux/atk-mp1/linux/my_linux/linux-5.4.31/drivers/usb/serial/usb_wwan.c` | 修正 Quectel zero packet 判断时必须比较 `idProduct`，不能误用 `iProduct` 字符串描述符索引。 |
| 虚拟机 Buildroot：`/home/cfr/linux/buildroot/buildroot-2020.02.6/configs/stm32mp1_atk_defconfig` | 持久化 `pppd`、`chat`、curl、OpenSSL、CA 证书和 rootfs overlay 配置。 |
| Buildroot overlay：`/home/cfr/linux/buildroot/buildroot-2020.02.6/board/stm32mp1_atk/rootfs_overlay/etc/ppp/quectel/ppp-on` | 标准 PPP 启动脚本，进入 `/etc/ppp/quectel` 并执行 `pppd file quectel_options connect chat ...`。 |
| Buildroot overlay：`/home/cfr/linux/buildroot/buildroot-2020.02.6/board/stm32mp1_atk/rootfs_overlay/etc/ppp/quectel/quectel_options` | PPP 主配置文件，指定 `/dev/ttyUSB2`、`115200`、`crtscts`、`persist`、`noauth`、`defaultroute`、`usepeerdns` 等选项。 |
| Buildroot overlay：`/home/cfr/linux/buildroot/buildroot-2020.02.6/board/stm32mp1_atk/rootfs_overlay/etc/ppp/quectel/quectel_ppp_dialer` | chat 拨号脚本，发送 `ATE`、`ATH`、`AT+CGDCONT=1,"IP","CTNET"`、`ATD*99#` 并等待 `CONNECT`；当前 EC20F 不发送 `ATP`。 |
| Buildroot overlay：`/home/cfr/linux/buildroot/buildroot-2020.02.6/board/stm32mp1_atk/rootfs_overlay/etc/ppp/quectel/disconnect` | 停止 PPP 时调用 `killall pppd`，释放 `/dev/ttyUSB2` 和 `ppp0`。 |
| Buildroot overlay：`/home/cfr/linux/buildroot/buildroot-2020.02.6/board/stm32mp1_atk/rootfs_overlay/usr/bin/4g-ppp` | 板端统一管理脚本，提供 start/stop/restart/status/test，等待 `ppp0`，检查路由和联网。 |
| Buildroot overlay：`/home/cfr/linux/buildroot/buildroot-2020.02.6/board/stm32mp1_atk/rootfs_overlay/usr/bin/board-time-sync` | 联网后校准北京时间，优先 `ntpd`，再 `rdate`，最后用 HTTP Date 头兜底，并写回 RTC。 |
| Buildroot overlay：`/home/cfr/linux/buildroot/buildroot-2020.02.6/board/stm32mp1_atk/rootfs_overlay/etc/profile.d/board-timezone.sh` | 登录 shell 时导出 `TZ=CST-8`，让人工查看 `date` 默认显示北京时间。 |
| Buildroot overlay：`/home/cfr/linux/buildroot/buildroot-2020.02.6/board/stm32mp1_atk/rootfs_overlay/etc/init.d/S80ppp-4g` | 开机自启动入口；拨号失败时会影响启动耗时，调试阶段可临时改名禁用。 |
| NFS rootfs：`/home/cfr/linux/nfs/rootfs/etc/ppp/quectel/` | 开发板当前实际使用的 PPP 配置目录。 |
| NFS rootfs：`/home/cfr/linux/nfs/rootfs/usr/bin/4g-ppp` | 开发板当前实际使用的 PPP 管理命令。 |
| NFS rootfs：`/home/cfr/linux/nfs/rootfs/usr/bin/board-time-sync` | 开发板当前实际使用的联网校时命令。 |
| NFS rootfs：`/home/cfr/linux/nfs/rootfs/etc/profile.d/board-timezone.sh` | 开发板当前实际使用的登录时区配置。 |
| NFS rootfs：`/home/cfr/linux/nfs/rootfs/etc/init.d/S80ppp-4g` | 开发板当前实际使用的开机自启动脚本。 |
| 本地规范：`.trellis/spec/backend/embedded-linux-workflow.md` | 沉淀 4G PPP、路由、COS HTTPS、天线缺失误判、rootfs overlay 持久化等执行契约。 |
| 本地模块文档：`22_4g_ppp/README.md` | 汇总本模块改动路径、修改原因、使用方法、验证证据和排障规则。 |
| 本地回归测试：`22_4g_ppp/test_board_time_sync.sh` | 检查校时脚本、4G 联动调用点、USB runtime power 保护和文档契约，防止以后删漏。 |
| 本地校时脚本：`22_4g_ppp/board-time-sync` | HTTP Date 兜底源同步为迁移后的 `http://139.9.35.72/health`，保持 4G 联网后轻量校时路径可用。 |
| 本地登录时区脚本：`22_4g_ppp/board-timezone.sh` | 防止人工登录后裸 `date` 仍显示 UTC，导致误判校时失败。 |
| 本地定位脚本：`22_4g_ppp/4g-location` | 改为高德 IP 省份定位入口，只调用 `AMAP_IP_URL=https://restapi.amap.com/v3/ip`；成功时写入 `state=ip_ok`，`display/short_display/province` 都为省份文本，例如 `河南省`，`city/district/longitude/latitude/satellites` 保持为空。 |
| 板端本地 Key 文件：`/etc/4g-location/amap-web-key` | 保存高德 Web 服务 Key 的本地私有文件；也可用环境变量 `AMAP_WEB_KEY` 临时传入。该文件不得提交到 Git。 |

## 修改原因

| 问题 | 原因 | 修改方式 |
|---|---|---|
| 模块只枚举成 USB 设备但没有 `/dev/ttyUSB*` | 内核缺少 USB serial option/WWAN 或模块 VID/PID 未匹配 | 启用 `CONFIG_USB_SERIAL*` 并在 `option.c` 中维护 modem 匹配。 |
| `pppd`/`chat` 命令缺失 | Buildroot rootfs 未启用 PPP 工具 | 在 Buildroot defconfig 中启用 `BR2_PACKAGE_PPPD=y` 并重建/部署 rootfs。 |
| `ppp0` 已起来但无法上网 | `pppd` 没替换旧的 `eth0` 默认路由 | 读取 `ifconfig ppp0` 当前本地 IP 后添加 PPP 默认路由，并保留 NFS/LAN 所需 eth0。 |
| COS/HTTPS 访问失败 | 只有 ping 不代表 HTTPS 能用，curl/OpenSSL/CA/时间都可能缺失 | 启用 curl、OpenSSL、CA 证书，检查 `date` 和 `curl -I https://cloud.tencent.com`。 |
| RTC 有纽扣电池但仍会漂移 | RTC 只能保持断电期间的时间基准，长期运行会有误差 | 4G 联网成功后自动执行 `board-time-sync once`，从网络校准系统时间，再 `hwclock -w -u` 写回 RTC。 |
| 登录后裸 `date` 仍显示 UTC | BusyBox 登录 shell 没有自动把 `/etc/TZ` 导出到 `TZ` 环境变量 | 部署 `/etc/profile.d/board-timezone.sh`，新登录 shell 自动 `export TZ=CST-8`。当前会话可手工执行 `. /etc/profile.d/board-timezone.sh`。 |
| rootfs 重建后脚本丢失 | 只把脚本复制到当前 NFS rootfs，没有放入 Buildroot overlay | 将 `/usr/bin/4g-ppp`、`S80ppp-4g` 和 `/etc/ppp/quectel/*` 放入 `board/stm32mp1_atk/rootfs_overlay`。 |
| IPCP 阶段 `Modem hangup` 被误判成脚本错误 | 没插 4G 天线时仍可能 AT、CONNECT、LCP、CHAP 成功，但拿 IP 时失败 | 先检查天线、信号、注册和 `AT+CEER`，不要先改 PPP 脚本。 |
| `AT+CPIN?` 返回 `+CME ERROR: 10` | SIM 卡可能刚拔出、接触未稳定，或模块没有重新完成 SIM 初始化 | 先运行 `4g-ppp monitor-start` 交给监控状态机处理；插回后用 `4g-ppp sim-status` 看是否进入 `+CPIN: READY`。如果长期仍为 `+CME ERROR: 10`，再检查卡槽接触、卡方向、天线和是否需要模块电源/USB 复位。 |
| `AT+CPIN?` 返回 `+CME ERROR: 13` | 2026-06-08 现场拔卡再插回后，`/dev/ttyUSB2` 和 `/dev/ttyUSB3` 都能回 `AT/OK`，但 `AT+CPIN?` 返回 `+CME ERROR: 13`；这不是 AT 口不存在，而是 SIM 初始化/卡接触/模块内部 SIM 状态错误 | 脚本将其归一化为 `sim_error`，默认允许按冷却时间做一次低频 `AT+CFUN=0/1` 重新初始化；如果仍不恢复，优先查卡座接触、卡方向、SIM 卡和模块供电。 |
| 旧版 `4g-location` 容易和 SIM 查询抢 AT 口 | 旧实现会打开 GPS 并访问 `/dev/ttyUSB3` 等 AT 口，可能和 `4g-ppp monitor` 的 SIM 查询串扰 | 现版 `4g-location` 已删除 GPS/AT 路径，只走高德 IP 定位 HTTPS 请求；定位失败只影响省份显示，不影响 PPP 拨号和 SIM 热插拔恢复。 |
| 插回 SIM 后恢复约 40 秒以上 | 现场日志显示 `18:42:40` 执行 `AT+CFUN=0/1`，直到 `18:43:26` 才 `AT+CPIN? -> READY`，真正耗时点是模块重新初始化 SIM/RF，不是 PPP 拨号；PPP 从 `READY` 到拿 IP 只约 3 秒 | `sim_error` 不再立即 CFUN，而是先进行 `SIM_ERROR_FAST_WAIT_SECONDS=10` 的轻量快速确认；只有未自然 READY 才 CFUN。CFUN 后不再等下一轮 8 秒轮询，而是在 `SIM_AFTER_CFUN_READY_WAIT_SECONDS=45` 内 1 秒一次等待 READY，READY 后立即重拨。 |
| 连续第二次拔插恢复接近 5 分钟 | 2026-06-08 现场第二轮日志显示第一次 CFUN 在 `19:07:08`，第二次直到 `19:12:19` 才执行，正好命中旧的 `SIM_CFUN_COOLDOWN_SECONDS=300` 冷却；期间脚本一直在 `sim_error` 低频检测，但不允许再次 CFUN | `sim_error` 单独使用 `SIM_CFUN_SIM_ERROR_COOLDOWN_SECONDS=30`，并在 PPP 成功上线后调用 `clear_sim_recovery_stamps` 清理恢复戳；长期无卡仍保留 300 秒 CFUN 冷却，避免没卡时反复重置模块。 |
| 拔卡后插回一直显示 `unknown` | `/dev/ttyUSB2` 仍是 PPP 拨号口，但热插拔后 AT 查询响应可能出现在其它 `ttyUSB` 管理口，或原 AT 口短时间不响应；旧脚本还没有识别 `+CME ERROR: 13` | `4g-ppp sim-status` 会按 `AT_TTY_CANDIDATES` 自动探测 `/dev/ttyUSB2 /dev/ttyUSB1 /dev/ttyUSB0 /dev/ttyUSB3`，状态文件会记录 `at_tty=`；`+CME ERROR: 13` 会进入 `sim_error` 恢复路径，不再无限 `unknown`。 |
| chat 在 `ATP` 处返回 `ERROR` | 当前 EC20F 不需要也不接受 `ATP` 脉冲拨号模式命令 | 从 `quectel_ppp_dialer` 删除 `OK ATP`，并同步 NFS rootfs 与 Buildroot overlay，避免重启或重建后回退。 |
| 4G USB 反复断开重连 | USB hub 或模块 runtime suspend、供电抖动、线缆/模块接触不稳都可能让 `/dev/ttyUSB2` 消失，pppd 随后 `Modem hangup` | `4g-ppp` 启动、状态和测试前先把 4G 模块及父级 hub 的 `power/control` 设为 `on`；如果仍断连，优先查 4G 模块供电电流、USB hub、线缆和模块硬件。 |
| 触摸和 4G 同时异常 | 触摸 I2C `-6` 与 4G USB disconnect 同时出现时，通常说明整板供电或外设负载已经影响多条硬件链路 | 先补足电源、减少 USB 负载、检查线缆/Hub/模块接触，再看 PPP 或触摸驱动日志；不要只改 APN、DNS、路由或触摸地址。 |
| Qt 只需要显示省份 | 城市和区县会被运营商出口 IP 误导，例如人在南阳但出口 IP 可能映射到郑州 | `4g-location once` 只显示高德 IP 定位返回的省份，成功状态为 `state=ip_ok`，示例 `display=河南省`；城市、区县和 GPS 坐标字段保持为空。 |

## 使用流程

### 内核与 rootfs 构建部署

| 阶段 | 命令 | 预期结果 |
|---|---|---|
| 进入内核树 | `cd /home/cfr/linux/atk-mp1/linux/my_linux/linux-5.4.31` | 位于 Linux 5.4.31 内核源码。 |
| 编译内核镜像 | `PATH=/usr/local/arm/gcc-arm-9.2-2019.12-x86_64-arm-none-linux-gnueabihf/bin:$PATH make uImage LOADADDR=0xC2000040 -j8` | 生成包含 USB serial/PPP 支持的 `uImage`。 |
| 部署内核和设备树 | `sudo cp arch/arm/boot/uImage arch/arm/boot/dts/stm32mp157d-atk.dtb /home/cfr/linux/tftpboot/ -f` | 开发板下一次网络启动加载新镜像/DTB。 |
| 进入 Buildroot | `cd /home/cfr/linux/buildroot/buildroot-2020.02.6` | 位于 Buildroot 2020.02.6。 |
| 编译 rootfs | `make -j8` | 生成包含 PPP、curl、OpenSSL、CA 和 overlay 脚本的 rootfs。 |
| 部署 rootfs | `cd output/images && sudo tar -axvf rootfs.tar -C /home/cfr/linux/nfs/rootfs` | NFS rootfs 更新。 |

### 板端拨号和联网

| 场景 | 命令 | 预期结果 |
|---|---|---|
| 检查 USB modem | `dmesg | grep -Ei "ttyUSB|GSM|option|ppp"` | 看到 `ttyUSB0` 到 `ttyUSB3` 和 PPP 驱动日志。 |
| 检查 USB runtime power | `cat /sys/bus/usb/devices/2-1/power/control /sys/bus/usb/devices/2-1.7/power/control 2>/dev/null` | 运行 `4g-ppp status` 或 `4g-ppp test` 后应输出 `on`。 |
| 检查串口节点 | `ls -l /dev/ttyUSB*` | 至少有 `/dev/ttyUSB2`。 |
| 检查 SIM 上电识别 | `printf 'AT+CPIN?\rAT+CREG?\rAT+CGREG?\rAT+CEREG?\r' > /dev/ttyUSB2; timeout 4 cat /dev/ttyUSB2` | `CPIN` 不应返回 `+CME ERROR: 10`，注册状态不应长期停在搜索中。 |
| 管理脚本启动 | `4g-ppp start` | 创建 `ppp0` 并尝试添加 PPP 默认路由。 |
| 查看状态 | `4g-ppp status` | 显示 `ppp0`、路由表和 pppd PID。 |
| 连接测试 | `4g-ppp test` | 能 ping 或 curl 指定外网目标。 |
| 手动前台排障 | `cd /etc/ppp/quectel && pppd file /etc/ppp/quectel/quectel_options connect 'chat -s -v -f /etc/ppp/quectel/quectel_ppp_dialer' nodetach debug` | 直接看到 `CONNECT`、LCP、CHAP、IPCP 等完整日志。 |
| HTTPS/COS 前置验证 | `curl -I https://cloud.tencent.com` | 返回 HTTP 状态，证明 DNS、HTTPS、CA 和时间基本可用。 |
| 手动校时 | `board-time-sync once; board-time-sync status` | `/etc/TZ` 为 `CST-8`，`board-time-sync status` 中的本地时间显示北京时间，`date -u` 显示 UTC，`hwclock -r` 可读，日志写入 `/var/log/board-time-sync.log`。 |
| 当前 shell 应用北京时间显示 | `. /etc/profile.d/board-timezone.sh; date` | 裸 `date` 显示 `CST` 和北京时间。 |
| 启动 SIM/PPP 自动恢复监控 | `4g-ppp monitor-start; 4g-ppp status` | 后台 monitor 运行，`/var/run/4g-ppp.state` 记录当前状态。 |
| 开机入口兼容测试 | `/etc/init.d/S80ppp-4g start; sleep 2; 4g-ppp status` | 既尝试一次 PPP 拨号，也能看到 monitor 状态文件；开机时没卡也不会高频检测。 |
| 查看 SIM 热插拔状态 | `4g-ppp sim-status` | pppd 未运行时输出 `at_tty=`、归一化 SIM 状态和 `AT+CPIN?` 原始响应。 |
| 调整长期无卡轮询频率 | `SIM_LONG_NO_CARD_INTERVAL=8 4g-ppp monitor-start` | 长时间未插卡后，主动 `AT+CPIN?` 兜底查询按 8 秒一次执行。 |

### 4G IP 省份定位

| 场景 | 命令 | 预期结果 |
|---|---|---|
| 临时配置高德 Key | `export AMAP_WEB_KEY='你的高德Web服务Key'` | 当前 shell 后续执行 `4g-location once` 可访问高德 IP 定位接口；Key 不写入 Git。 |
| 持久配置高德 Key | `mkdir -p /etc/4g-location; printf '%s\n' '你的高德Web服务Key' > /etc/4g-location/amap-web-key; chmod 600 /etc/4g-location/amap-web-key` | Key 只保存在板端本地，`4g-location` 会优先用 `AMAP_WEB_KEY`，其次读取该文件。 |
| 执行一次定位 | `4g-location once; echo "exit=$?"; cat /var/run/4g-location.state` | 高德 IP 定位成功时，状态文件包含 `state=ip_ok`、`display=河南省`、`short_display=河南省`、`province=河南省`、`city=`、`district=`、`longitude=`、`latitude=`、`satellites=` 和 `updated_at=`。 |
| 查看缓存定位状态 | `4g-location status` | 输出最近一次 `/var/run/4g-location.state`；不会主动访问网络、GPS 或 AT 串口。 |
| 缺少 Key 验证 | `unset AMAP_WEB_KEY; rm -f /etc/4g-location/amap-web-key; 4g-location once; cat /var/run/4g-location.state` | 输出 `state=no_key`、`display=缺少高德Key`，Qt 会显示 `缺少Key`。 |

### 临时禁用开机拨号

调试阶段如果 4G 没天线、没卡或信号差，开机自启动会等待失败，导致登录变慢。可临时禁用：

```bash
mv /etc/init.d/S80ppp-4g /etc/init.d/K80ppp-4g
```

恢复开机拨号：

```bash
mv /etc/init.d/K80ppp-4g /etc/init.d/S80ppp-4g
```

## 修改记录

| 时间 | 修改点 | 结果 |
|---|---|---|
| 2026-05-02 | 补齐内核 USB serial option、PPP 相关配置和 Quectel modem 支持 | 开发板能识别 4G modem 并生成 `/dev/ttyUSB0~3`。 |
| 2026-05-02 | 在 rootfs 中部署 `/etc/ppp/quectel` 标准 PPP 拨号文件 | `./ppp-on &` 可执行 AT 初始化、设置 `CTNET`、拨号 `ATD*99#`。 |
| 2026-05-02 | 新增 `4g-ppp` 管理脚本和 `S80ppp-4g` 自启动脚本 | 支持统一 `start/stop/restart/status/test`，并能开机自动拨号。 |
| 2026-05-02 | 增加 curl/OpenSSL/CA 证书等 COS HTTPS 前置依赖 | 4G 联网后可继续验证腾讯云 HTTPS 上传。 |
| 2026-05-03 | 排查开机慢和 `Starting 4G PPP: FAIL` | 确认为开机 PPP 自启动等待失败后继续登录，不是系统死机。 |
| 2026-05-03 | 前台 `pppd` 复现 `CONNECT`、CHAP 成功后 IPCP `Modem hangup` | 根因确认为 4G 天线未插，脚本本身没有错误。 |
| 2026-05-03 | 更新 `.trellis/spec/backend/embedded-linux-workflow.md` | 写入“缺天线会导致 IPCP Modem hangup，不要误改脚本”的排障规则。 |
| 2026-05-08 | 修正 EC20F chat 拨号脚本，不再发送 `ATP` | 现场日志显示 `ATP` 返回 `ERROR` 会导致 `Connect script failed`；删除后可继续到 `AT+CGDCONT`、`ATD*99#`、`CONNECT` 和 CHAP。 |
| 2026-05-08 | 记录 SIM 上电前插入要求 | 未上电前插卡时，模块能生成 `/dev/ttyUSB0~3`，但 `AT+CPIN?` 返回 `+CME ERROR: 10`、注册状态停在 `0,2`，重新上电后 `ppp0` 成功上线。 |
| 2026-05-08 | 增加 4G 联网后自动校时 | 新增 `board-time-sync`，`4g-ppp start/restart` 在 `ppp0` 在线并处理默认路由后自动调用一次；校时成功后写回 RTC，校时失败只记日志，不误判 PPP 拨号失败。 |
| 2026-05-08 | 增加登录时区脚本 | 新增 `board-timezone.sh` 并部署到 `/etc/profile.d/`，解决 root 登录后直接执行 `date` 仍显示 UTC 的误导问题。 |
| 2026-05-20 | 增加 USB runtime power 固定逻辑 | `4g-ppp` 在 start/status/test 和等待 ttyUSB 时写 `power/control=on`；现场 30 秒观察 `usb 2-1.7` 断连计数未增加，`4g-ppp test` 退出码为 0。 |
| 2026-05-20 | 补足整板供电后复测 4G 稳定 | `ppp0` 在线，默认路由指向 `ppp0`，日志持续显示 USB 供电常开和默认路由无需重复添加；用户确认 4G 模块不再时不时断线。 |
| 2026-06-08 | 云服务器迁移后更新 HTTP Date 兜底源 | `board-time-sync` 默认 `HTTP_TIME_URLS` 改为 `http://139.9.35.72/health https://cloud.tencent.com`，账号和 COS 上传接口不变。 |
| 2026-06-08 | 增加 SIM 热插拔自适应恢复状态机 | 根据原理图确认 `USIM_PRESENT` 没有接到 Nano SIM 卡座检测脚，新增 `sim-status`、`monitor-start`、`monitor-stop` 和 `/var/run/4g-ppp.state`；无卡长期状态按 `SIM_LONG_NO_CARD_INTERVAL` 默认 8 秒低频轮询，插回后短时高频确认 `AT+CPIN? -> READY` 并自动重拨。 |
| 2026-06-08 | 修复 SIM 插回后长期 `unknown` 的检测路径 | 保留 `/dev/ttyUSB2` 作为 PPP 拨号口，同时新增 `AT_TTY_CANDIDATES` 自动探测 AT 查询口；`sim-status` 和 monitor 会记录 `at_tty=`，避免只盯固定 ttyUSB2 导致插回 SIM 后一直检测不到。 |
| 2026-06-08 | 识别 `+CME ERROR: 13` 并增加在线 SIM/链路健康检测 | 现场 raw AT 证据显示插回 SIM 后返回 `+CME ERROR: 13`；脚本新增 `sim_error` 状态、默认低频 CFUN 兜底，以及 `SIM_ONLINE_SIM_CHECK_INTERVAL=30` 在线辅助 AT 口检测。若没有辅助 AT 口，则只用 `ping -I ppp0` 做 4G 链路健康检查，不退化为普通 ping，避免 eth0/NFS 误判 4G 在线。 |
| 2026-06-08 | 优化 `sim_error` 恢复速度 | 根据板端日志确认慢点在 `AT+CFUN=0/1` 后等待模块重新 READY，而不是 PPP 拨号。新增 `SIM_ERROR_FAST_WAIT_SECONDS`、`SIM_ERROR_FAST_COOLDOWN_SECONDS` 和 `SIM_AFTER_CFUN_READY_WAIT_SECONDS`：先短时轻量确认，失败再 CFUN；CFUN 后快速等待 READY 并立即重拨，保持长期无卡轮询仍为 8 秒。 |
| 2026-06-08 | 修复连续第二次拔插被 CFUN 冷却拖慢 | 现场第二轮恢复慢到约 5 分钟，根因为 `sim_error` 与长期无卡共用 `SIM_CFUN_COOLDOWN_SECONDS=300`。新增 `SIM_CFUN_SIM_ERROR_COOLDOWN_SECONDS=30`，PPP 成功上线后清理恢复时间戳，连续拔插测试不会被上一轮 CFUN 冷却卡住。 |
| 2026-06-08 | 新增 4G 位置状态文件 | 新增 `4g-location`，独立于 `4g-ppp` 状态机，并写 `/var/run/4g-location.state` 供 Qt 显示。 |
| 2026-06-09 | 定位改为高德 IP 省份显示 | 删除 `4g-location` 的 GPS/AT 串口路径，只调用 `AMAP_IP_URL=https://restapi.amap.com/v3/ip`；成功后写入 `state=ip_ok`，并且只显示省份，例如 `河南省`。 |

## 硬件资源

| 资源 | 用途 | 契约 |
|---|---|---|
| EC20/Quectel 4G 模块 | PPP 数据链路 | 插好 SIM 卡并接好天线后才能稳定注册运营商网络；新版脚本支持拔卡后重新插回自动检测和重拨，但卡槽接触、供电和天线仍是硬件前置条件。 |
| `USIM_PRESENT` | SIM 插拔检测输入 | 原理图中 U22 引脚已画出，但没有接到 Nano SIM 卡座检测脚；默认 `SIM_PRESENT_WIRED=0`，不能依赖硬件插卡中断，只能用无卡低频轮询和可能出现的串口 URC。 |
| Nano SIM 卡座 U26 | SIM 接口 | 只连接 `SIM_VDD`、`SIM_RST`、`SIM_CLK`、`SIM_DATA` 和 `GND`，未提供独立卡在位检测开关给 U22 或 MP157 GPIO。 |
| `/dev/ttyUSB2` | PPP 拨号和数据端口 | 当前确认拨号配置使用该端口，不要无证据改成其它 ttyUSB。SIM 状态查询会先用 `AT_TTY_CANDIDATES` 自动找可响应 `AT` 的管理口，避免把 AT 查询口和 PPP 拨号口硬绑死。 |
| `AT_TTY_CANDIDATES` | SIM 状态查询候选口 | 默认顺序为 `$PPP_TTY /dev/ttyUSB2 /dev/ttyUSB1 /dev/ttyUSB0 /dev/ttyUSB3`；monitor 会选择能返回 `OK` 的口发送 `AT+CPIN?`，状态文件写入 `at_tty=`。 |
| 高德 Web 服务 Key | 高德 IP 省份定位 | 只通过 `AMAP_WEB_KEY` 或 `/etc/4g-location/amap-web-key` 提供；不要写入 Git。`4g-location` 只调用高德 IP 定位接口，并且只显示省份。 |
| `/var/run/4g-location.state` | 定位状态缓存 | 保存 `state/display/short_display/province/city/district/longitude/latitude/satellites/updated_at/detail`，Qt 异步读取脚本输出和状态文件显示位置。 |
| `/sys/bus/usb/devices/2-1/power/control` | 4G 所在父级 USB hub runtime power 控制 | `4g-ppp` 默认写 `on`，防止父级 hub 自动挂起影响子设备。 |
| `/sys/bus/usb/devices/2-1.7/power/control` | 4G 模块 USB 设备 runtime power 控制 | `4g-ppp` 默认写 `on`，减少 `usb 2-1.7: USB disconnect` 导致的网络离线。 |
| 中国电信 APN | PDP 上下文 | 当前普通电信卡使用 `AT+CGDCONT=1,"IP","CTNET"`。 |
| `ppp0` | 4G 网络接口 | 成功后应有 `UP POINTOPOINT RUNNING` 和运营商分配的本地 IP。 |
| `eth0` | NFS/LAN 网络 | NFS 调试时不要随意断开；4G 默认路由调整要兼顾 LAN/NFS。 |
| 天线 | 无线射频链路 | 缺天线时可能仍 `CONNECT` 和 CHAP 成功，但 IPCP 拿 IP 时 `Modem hangup`。 |

## 验证证据

| 验证项 | 已确认结果 |
|---|---|
| USB 枚举 | `option ... GSM modem ... ttyUSB0~ttyUSB3`。 |
| PPP 内核支持 | `PPP generic driver version 2.4.2`、BSD/Deflate/MPPE 模块日志存在。 |
| 成功拨号记录 | `/etc/ppp/quectel/ppp-on &` 曾创建 `ppp0`，本地 IP `10.144.240.122`，DNS `222.85.85.85/222.88.88.88`。 |
| 路由修复 | `route add default gw <current-ppp0-local-ip>` 后外网 ping 成功。 |
| HTTPS 前置 | `curl --version` 需包含 `OpenSSL` 和 `https`，`curl -I https://cloud.tencent.com` 应返回 HTTP 状态。 |
| 联网校时 | 4G 成功后执行 `board-time-sync once`，设置 `/etc/TZ=CST-8`，用 `ntpd`/`rdate`/HTTP Date 获取网络时间，并执行 `hwclock -w -u`。 |
| 本次误判排除 | 直接执行教材式 `./ppp-on &` 也在 IPCP 阶段 `Modem hangup`，证明不是 `4g-ppp` 包装脚本导致。 |
| 天线根因 | 用户确认没有插天线；插天线后应优先重新测试，不应先修改 PPP 脚本。 |
| 2026-05-08 成功联网 | SIM 上电前插好后，`ppp0` 获取 `10.35.53.241`，后端 `http://119.91.65.122` 和 `https://cloud.tencent.com` 均返回 HTTP 200。 |
| 2026-05-08 COS 上传前置 | `defect-cos-upload --self-test-json-parser` 通过；保存按钮生成新云端记录 `record_id=9`、`record_no=MP157-20260508-130113`，详情只含 `source` 和 `annotated` 两个文件。 |
| 2026-05-08 校时工具探测 | 板端有 `/sbin/hwclock`、BusyBox `rdate`、curl/OpenSSL，当前未发现 `ntpd`，因此校时脚本必须保留 rdate 和 HTTP Date 兜底路径。 |
| 2026-05-20 USB 断连观察 | `usb 2-1.7: USB disconnect` 反复出现时，`pppd` 会 `Modem hangup`，Qt 按真实 `4g-ppp test` 显示离线；设置父 hub 和 4G 设备 `power/control=on` 后，30 秒内断连计数 `delta_disc=0`，`4g-ppp test` 返回 `exit=0`。补足整板供电后，`ppp0` 在线且日志持续显示默认路由已经指向 `ppp0`，用户确认 4G 不再时不时断线。 |
| 2026-06-08 原理图检查 | U22 `USIM_PRESENT` 没有接到 Nano SIM 卡座检测脚，U26/U27 只承载 SIM 电源、复位、时钟、数据和 ESD 保护，因此软件不能承诺长期无卡时零轮询即时发现，只能通过低频轮询加 URC 被动监听提高插回响应速度。 |
| 2026-06-08 定位显示契约 | `4g-location` 采用独立状态文件，不影响 `4g-ppp` 拨号和 SIM 恢复；高德 Key 不写入源码，Qt 只显示 `locationStatusText/locationDisplayText/locationShortText`。 |

## 新版测试与验证矩阵

| Test goal | Run location | Command | Expected result | Failure triage |
|---|---|---|---|---|
| USB modem 枚举 | 开发板 | `dmesg | grep -Ei "ttyUSB|GSM|option|Quectel"; ls -l /dev/ttyUSB*` | 出现 `/dev/ttyUSB0~3`，其中 `/dev/ttyUSB2` 可用于 PPP。 | 若无 ttyUSB，查 USB 供电、option 驱动、VID/PID 和内核配置。 |
| USB runtime power 保护 | 开发板 | `4g-ppp status; cat /sys/bus/usb/devices/2-1/power/control /sys/bus/usb/devices/2-1.7/power/control 2>/dev/null` | 两个存在的 `power/control` 文件应显示 `on`，`dmesg` 不应继续快速增加 `usb 2-1.7: USB disconnect`。 | 如果仍断连，先查 4G 模块供电、USB hub、线缆和模块接触；软件只能重拨和禁止 runtime suspend，不能修复瞬断供电。 |
| 供电充足后稳定性复测 | 开发板 | `ifconfig ppp0; route -n; tail -n 80 /tmp/4g-ppp.log; dmesg \| grep -Ei "ttyUSB|disconnect|Modem hangup" \| tail -n 80` | `ppp0` 为 `UP POINTOPOINT RUNNING`，默认路由包含 `ppp0`，近期日志不再快速出现 USB disconnect 或 pppd 反复重拨。 | 若补足供电后仍断线，优先查 4G 模块峰值电流、USB hub 供电、线缆压降和模块接触，再排查 APN/路由。 |
| SIM 状态手动查询 | 开发板 | `4g-ppp stop; 4g-ppp sim-status` | `pppd` 停止后，命令输出 `at_tty=`、归一化状态和 `AT+CPIN?` 原始响应；插卡稳定后应看到 `ready` 或 `+CPIN: READY`。 | 如果显示 `pppd_busy`，先 `4g-ppp stop`；如果没有 `at_tty=`，查 `/dev/ttyUSB*` 是否存在、供电和 USB 枚举；如果长期 `no_sim` 或 `+CME ERROR: 10`，查卡方向、卡座接触和模块是否需要复位。 |
| 在线拔卡离线切换 | 开发板 | `4g-ppp monitor-start; tail -f /var/log/4g-ppp.log`，在 `ppp0` 在线时拔出 SIM | monitor 每 `SIM_ONLINE_SIM_CHECK_INTERVAL` 默认 30 秒尝试非 `/dev/ttyUSB2` 辅助 AT 口检测 SIM；确认 `no_sim` 或 `sim_error` 后主动停止 pppd 并写离线状态。没有辅助 AT 口时，继续用 `ping -I ppp0` 检测 4G 链路，失败后写 `link_offline`。 | 如果 30 秒后仍在线，查 `/dev/ttyUSB3` 是否可回 `AT/OK`、`ping -I ppp0 cloud.tencent.com` 是否真的还能通，以及 Qt 是否读取 `4g-ppp status/test` 的最新结果；不要用不带 `-I ppp0` 的普通 ping 判断 4G 在线。 |
| SIM 热插拔监控启动 | 开发板 | `4g-ppp monitor-start; sleep 2; 4g-ppp status; cat /var/run/4g-ppp.state` | `monitor` PID 存在，状态文件包含 `state=`、`detail=`、`ppp_tty=/dev/ttyUSB2`、`at_tty=`。 | 如果 PID 不存在，查 `/var/log/4g-ppp.log`；如果状态文件不可写，查 `/var/run` 权限和根文件系统状态。 |
| 无卡低频轮询 | 开发板 | `4g-ppp monitor-start; tail -f /var/log/4g-ppp.log`，拔出 SIM 后观察日志 | 刚拔卡后日志显示 5 秒短间隔等待；持续超过 `SIM_RECENT_NO_CARD_WINDOW_SECONDS` 默认 8 秒后，日志中的“本轮低频等待”应变为 `SIM_LONG_NO_CARD_INTERVAL` 默认 8 秒，证明无卡时没有每秒高频抢占。 | 如果每秒持续查询，检查 `SIM_LONG_NO_CARD_INTERVAL` 是否被错误设置过小；如果完全无日志，查 monitor 是否运行。 |
| 插回快速确认 | 开发板 | `4g-ppp monitor-start` 后拔卡，等待超过 `SIM_LONG_NO_CARD_INTERVAL`，再插回 SIM，随后执行 `tail -n 160 /var/log/4g-ppp.log; cat /var/run/4g-ppp.state; ifconfig ppp0` | 插回后如果模块吐出 URC，monitor 立即进入快速确认；否则最多等到下一次低频兜底查询。确认 `AT+CPIN? -> READY` 后自动启动 PPP，`ppp0` 恢复在线；如果返回 `+CME ERROR: 13`，日志应先出现 `先快速确认`，未恢复才出现 `AT+CFUN=0/1`。CFUN 后应在 `SIM_AFTER_CFUN_READY_WAIT_SECONDS` 窗口内持续等待 READY，READY 后立即重拨。 | 如果插回后一直不恢复，先 `4g-ppp sim-status`；若仍 `+CME ERROR: 13`，查卡座接触、SIM 卡、供电和模块复位条件。 |
| 调整低频检测周期 | 开发板 | `4g-ppp monitor-stop; SIM_LONG_NO_CARD_INTERVAL=8 4g-ppp monitor-start; tail -n 80 /var/log/4g-ppp.log` | 长期无卡后主动兜底查询间隔按 8 秒执行。 | 如果仍按旧间隔，说明旧 monitor 未停止或启动命令没有带环境变量。 |
| 调整 `sim_error` 快速恢复窗口 | 开发板 | `4g-ppp monitor-stop; SIM_ERROR_FAST_WAIT_SECONDS=10 SIM_CFUN_SIM_ERROR_COOLDOWN_SECONDS=30 SIM_AFTER_CFUN_READY_WAIT_SECONDS=45 4g-ppp monitor-start; tail -n 120 /var/log/4g-ppp.log` | `sim_error` 后先短时快速确认；确认失败再 CFUN；CFUN 后 1 秒一次等待 READY，READY 后立即重拨。PPP 上线后会清理恢复时间戳，下一轮拔插可重新开始恢复流程。 | 如果仍一出现 `sim_error` 就等 300 秒才 CFUN，说明板端脚本不是新版或旧 monitor 没停止；先查 `sha256sum /usr/bin/4g-ppp` 和 `/var/run/4g-ppp-monitor.pid`。 |
| 高德 Key 文件权限 | 开发板 | `mkdir -p /etc/4g-location; printf '%s\n' "$AMAP_WEB_KEY" > /etc/4g-location/amap-web-key; chmod 600 /etc/4g-location/amap-web-key; ls -l /etc/4g-location/amap-web-key` | 输出权限为 `-rw-------`，Key 只保存在板端本地。 | 如果权限过宽，重新 `chmod 600`；如果要换 Key，直接覆盖该文件或设置环境变量。 |
| 4G IP 省份定位 | 开发板 | `4g-location once; echo "exit=$?"; cat /var/run/4g-location.state; tail -n 80 /var/log/4g-location.log` | 成功时输出 `state=ip_ok`，`display=河南省`，`short_display=河南省`，`province=河南省`；`city/district/longitude/latitude/satellites` 为空，表示只显示省份，不使用 GPS 坐标。 | 如果为 `no_key`，配置 `AMAP_WEB_KEY` 或 `/etc/4g-location/amap-web-key`；如果为 `ip_failed`，查高德 Key 是否 Web 服务类型、`curl -I https://restapi.amap.com`、4G 出口网络和接口配额。 |
| 定位状态缓存读取 | 开发板 | `4g-location status; cat /var/run/4g-location.state` | 两者输出一致，且不会主动访问 AT 串口。 | 如果文件不存在，先执行 `4g-location once`；如果文件不可写，查 `/var/run` 权限。 |
| PPP 工具存在 | 开发板 | `command -v pppd; command -v chat; pppd --version 2>&1 | head -n 1` | `pppd` 和 `chat` 可执行。 | 若缺失，回 Buildroot 启用 `BR2_PACKAGE_PPPD` 并部署 rootfs。 |
| 配置文件存在 | 开发板 | `ls -l /etc/ppp/quectel/ppp-on /etc/ppp/quectel/quectel_options /etc/ppp/quectel/quectel_ppp_dialer` | 三个文件存在且脚本可执行。 | 若只在 NFS rootfs 手工复制，记得同步 Buildroot overlay，避免重建丢失。 |
| 前台拨号日志 | 开发板 | `cd /etc/ppp/quectel && pppd file /etc/ppp/quectel/quectel_options connect 'chat -s -v -f /etc/ppp/quectel/quectel_ppp_dialer' nodetach debug` | 能直接看到 AT、CONNECT、LCP、CHAP、IPCP 过程。 | 若 CHAP 后 `Modem hangup`，先查天线、信号、注册和 `AT+CEER`，不要先改脚本。 |
| 管理脚本启动 | 开发板 | `4g-ppp restart; 4g-ppp status` | `ppp0` 出现，显示 pppd PID、路由和接口信息。 | 若 `ppp0 未在线`，回到前台 `pppd ... nodetach debug` 看具体阶段。 |
| 联网后自动校时 | 开发板 | `4g-ppp restart; board-time-sync status; TZ=CST-8 date; date -u; hwclock -r; tail -n 40 /var/log/board-time-sync.log` | 日志显示 `板端网络校时完成` 或明确某个兜底源成功；`TZ=CST-8 date` 与北京时间同小时，`date -u` 为 UTC，`hwclock -r` 可读。 | 若校时失败，先查 `ppp0` 路由、DNS、curl、`/etc/TZ` 权限和 `/var/log/board-time-sync.log`，不要先改 COS 上传脚本。 |
| 登录时区显示 | 开发板新登录 shell | `echo "$TZ"; date; date -u` | `TZ` 为 `CST-8`，裸 `date` 显示 CST 北京时间，`date -u` 显示 UTC。 | 若 `TZ` 为空，执行 `. /etc/profile.d/board-timezone.sh` 验证脚本；再查 `/etc/profile` 是否遍历 `/etc/profile.d/*.sh`。 |
| IP 和路由 | 开发板 | `ifconfig ppp0; ip route show; cat /etc/resolv.conf` | `ppp0` 有本地 IP，路由和 DNS 可见。 | 若默认路由仍走 eth0，按当前 ppp0 IP 调整路由，同时保留 NFS 调试链路。 |
| 外网连通 | 开发板 | `4g-ppp test` | ping 或 curl 外网目标成功。 | 若 IP 可 ping 但域名失败，先查 DNS；若 HTTPS 失败，查 CA 和系统时间。 |
| HTTPS/COS 前置 | 开发板 | `date; curl --version; curl -I https://cloud.tencent.com` | 时间合理，curl 支持 HTTPS/OpenSSL，返回 HTTP 状态。 | 若证书错误，查 CA；若时间错误，先校时；若连接超时，查信号和路由。 |
| 手动校时 | 开发板 | `board-time-sync once; board-time-sync status; TZ=CST-8 date; date -u` | `/etc/TZ` 为 `CST-8`，显式 TZ 的 `date` 是北京时间，`date -u` 是 UTC 时间，日志写入 `/var/log/board-time-sync.log`。 | 若 `ntpd` 不存在是可接受情况，继续看 rdate/HTTP Date 兜底；若三者都失败，查 DNS、路由和时间源是否可达。 |
| 停止拨号 | 开发板 | `4g-ppp stop; 4g-ppp status` | pppd 退出，`ppp0` 消失或离线。 | 若 pppd 残留，查 `ps | grep pppd` 和 `/dev/ttyUSB2` 占用。 |

## 网络读写/数据路径和文件保存验证

| 数据路径 | 验证命令 | 通过标准 |
|---|---|---|
| 串口拨号控制 | `pppd ... nodetach debug` | 看到 AT 命令、`CONNECT`、LCP/CHAP/IPCP 阶段日志。 |
| PPP 网络接口 | `ifconfig ppp0` | `ppp0` 存在并有运营商分配 IP。 |
| 路由路径 | `ip route get 1.1.1.1` | 返回路径经 `ppp0` 或符合当前调试目标。 |
| DNS | `nslookup cloud.tencent.com` 或 `ping -c 1 cloud.tencent.com` | 域名能解析。 |
| HTTPS | `curl -I https://cloud.tencent.com` | 返回 HTTP 状态码。 |
| 网络校时 | `board-time-sync once; board-time-sync status; TZ=CST-8 date; date -u; hwclock -r` | 显式 TZ 的系统本地时间为北京时间，UTC 时间相差 8 小时，RTC 可读且已写回。 |
| 登录显示 | `. /etc/profile.d/board-timezone.sh; date` | 当前 shell 立即切换为 CST 北京时间显示；重新登录后无需手工 source。 |
| 高德 IP 省份定位读路径 | `4g-location once; grep -E 'state=ip_ok|province=|display=|short_display=|city=|district=|longitude=|latitude=|satellites=' /var/run/4g-location.state` | 高德 IP 定位成功时只写入省份；城市、区县、经纬度和卫星数字段为空，避免把运营商出口城市误当成板子真实位置。 |
| 定位配置写路径 | `printf '%s\n' "$AMAP_WEB_KEY" > /etc/4g-location/amap-web-key; chmod 600 /etc/4g-location/amap-web-key; 4g-location once` | Key 写入本地私有配置后，脚本可完成高德 IP 省份定位；Key 不进入源码仓库。 |
| COS 上传前本地文件 | `test -s /mnt/sdcard/images/test.jpg && stat -c %s /mnt/sdcard/images/test.jpg` | 文件存在且大小大于 0，且已完成 SD 卡等待流程。 |

如果后续通过 4G 上传 SD 卡里的图片到 COS，必须先确认图片已经写完，再开始上传：

```sh
file=/mnt/sdcard/images/test.jpg
while true; do
    s1=$(stat -c %s "$file" 2>/dev/null || echo 0)
    sleep 1
    s2=$(stat -c %s "$file" 2>/dev/null || echo 0)
    [ "$s1" = "$s2" ] && [ "$s1" -gt 0 ] && break
done
sync
curl -I https://cloud.tencent.com
# 这里再执行 COS 上传命令；上传后保留 HTTP 状态码和响应日志。
```

上传日志如果写入 SD 卡，同样要等待上传程序退出、日志大小稳定、`sync` 完成，再执行 `sdcard-safe-remove`。不要在 PPP 仍在写日志或图片文件仍在增长时断电。

## 排障规则

| 现象 | 先查什么 | 不要先做什么 |
|---|---|---|
| 开机卡在 `Starting 4G PPP: FAIL` 附近 | 是否已进入 `login:`，`4g-ppp status`，`ps | grep ppp` | 不要判断为内核死机。 |
| `AT+CPIN?` 返回 `+CME ERROR: 10` | SIM 是否在上电前插入、卡槽方向和接触、重新上电后的注册状态 | 不要先改 APN、拨号串口或内核驱动。 |
| 拔卡后长期无卡 | `cat /var/run/4g-ppp.state`、`tail -n 80 /var/log/4g-ppp.log`、`SIM_LONG_NO_CARD_INTERVAL` 是否过小 | 不要把无卡状态下的轮询间隔设成 1 秒；会抢 AT 串口并增加系统负担。 |
| 插回 SIM 后恢复慢 | 先看原理图硬件限制：`USIM_PRESENT` 没有接到 Nano SIM 卡座检测脚；再看日志是否收到 URC、下一次低频轮询间隔是多少，以及是否走了 `AT+CFUN=0/1`。如果日志显示 CFUN 后几十秒才 READY，慢点在模块重新初始化 SIM/RF | 不要承诺“长期无卡仍能瞬时发现”；没有卡在位检测脚时，最坏发现时间就是低频轮询间隔。也不要把长期无卡轮询改成 1 秒；应只优化 `sim_error` 恢复窗口。 |
| 插回 SIM 后一直不恢复 | `4g-ppp stop; 4g-ppp sim-status` 是否显示 `at_tty=` 和 `+CME ERROR: 13`、`cat /var/run/4g-ppp.state` 是否为 `sim_error`、卡座接触、SIM 卡方向、4G 供电、CFUN 冷却时间 | 不要无限重启 PPP；SIM 未 READY 时重拨只会反复失败，也不要直接把 PPP 拨号口从 `/dev/ttyUSB2` 改到其它口。 |
| chat 日志停在 `ATP` 并返回 `ERROR` | `/etc/ppp/quectel/quectel_ppp_dialer` 是否还保留 `OK ATP` | 不要为了绕过 `ATP` 失败去改 APN 或 ttyUSB；当前 EC20F 应删除 `OK ATP`。 |
| `ppp0 未在线` 但 `pppd` 还在 | 前台运行 `pppd ... nodetach debug` 看 `CONNECT/LCP/CHAP/IPCP` | 不要先改路由，因为 `ppp0` 还没拿到 IP。 |
| `CONNECT` 和 `CHAP authentication succeeded` 后 `Modem hangup` | 天线、`AT+CSQ`、`AT+CGREG?`/`AT+CEREG?`、`AT+CGATT?`、`AT+CEER` | 不要先改 `ppp-on`、`quectel_options` 或 APN。 |
| `usb 2-1.7: USB disconnect` 反复出现 | `cat /sys/bus/usb/devices/2-1/power/control /sys/bus/usb/devices/2-1.7/power/control`、4G 供电、USB hub、线缆、模块接触 | 不要只改 APN、DNS、路由或 Qt 状态显示；ttyUSB 瞬断时网络离线是正确显示。 |
| 触摸屏和 4G 在同一轮开机都异常 | 整板供电是否充足、USB 摄像头/4G 等外设总负载、5V/3.3V 电压跌落、触摸排线和 I2C 上拉 | 不要把两个独立外设同时异常先拆成两个软件 bug；先排除供电和硬件连接。 |
| `ppp0` 有 IP 但 ping 外网失败 | `ip route show` 是否仍优先走 `default via 192.168.1.1 dev eth0` | 不要说 PPP 没拨上。 |
| ping 域名失败但 IP 可通 | `/etc/resolv.conf`、`usepeerdns`、运营商 DNS | 不要先改内核或 USB 驱动。 |
| HTTPS/COS 失败 | `curl --version`、CA 证书、`date` 时间 | 不要用 BusyBox `wget` 的 HTTPS 限制判断 4G 坏了。 |
| `date` 时间漂移或裸 `date` 仍显示 `UTC` | `board-time-sync status`、`TZ=CST-8 date`、`date -u`、`cat /etc/TZ`、`tail /var/log/board-time-sync.log`、`hwclock -r` | 不要在 COS 上传脚本里手工加减 8 小时；业务脚本要显式导出 `TZ=CST-8`，同时先修系统校时和 RTC 写回。 |
| 新登录后 `TZ` 仍为空 | `/etc/profile` 是否加载 `/etc/profile.d/*.sh`、`ls -l /etc/profile.d/board-timezone.sh`、脚本内容是否 `export TZ=CST-8` | 不要怀疑 `board-time-sync` 校时失败；这是登录环境变量问题。 |
| `4g-location once` 返回 `ip_failed` | `4g-ppp test`、`curl -I https://restapi.amap.com`、`tail -n 120 /var/log/4g-location.log`、高德 Key 是否 Web 服务类型和配额是否用尽 | 不要排查 GPS 或 AT 串口；现版定位只走 HTTPS IP 定位接口。 |
| Qt 显示 `位置:缺少Key` | `cat /var/run/4g-location.state` 是否为 `state=no_key`、`echo "$AMAP_WEB_KEY"`、`ls -l /etc/4g-location/amap-web-key` | 不要把它当成 4G 离线；这是高德 Web 服务 Key 未配置或未被当前进程读取。 |
| Qt 只显示 `河南省` 这类省份 | `cat /var/run/4g-location.state` 是否为 `state=ip_ok` | 这是当前设计目标；不显示城市和区县，避免运营商出口城市误导。 |

## 本次 SIM 热插拔经验总结

| 经验项 | 结论 | 下次必须怎么做 |
|---|---|---|
| 根因分类 | 这次属于“硬件检测缺失 + 隐式冷却假设 + 集成测试覆盖不足”。原理图没有把 `USIM_PRESENT` 接到卡座检测脚，软件只能轮询/监听 URC；旧逻辑又让 `sim_error` 和长期无卡共用 300 秒 CFUN 冷却。 | 遇到热插拔问题时，先确认原理图有没有卡在位检测脚，再看日志时间线，不要直接调 APN、PPP 拨号口或默认路由。 |
| 日志时间线 | 第一次恢复慢点在 `AT+CFUN=0/1` 后等待 SIM READY；第二次恢复慢点在 300 秒 CFUN 冷却。PPP 从 READY 到拿 IP 通常只要几秒。 | 每次排障必须列出 `LCP TermReq`、`sim_error/no_sim`、`快速确认开始/超时`、`CFUN`、`READY`、`ppp0 已上线` 的绝对时间。 |
| 在线检测 | `/dev/ttyUSB2` 是 PPP 数据口，在线时不能抢；当前辅助 AT 口是 `/dev/ttyUSB3`。 | 在线拔卡判断要优先看 `/var/run/4g-ppp.state` 的 `at_tty=` 和日志中的 `在线 SIM 健康检测正常：/dev/ttyUSB3 -> READY`，不要用普通 ping 判断 4G 在线。 |
| 冷却策略 | 长期无卡需要长冷却，避免没卡时反复 CFUN；但 `sim_error` 是插回后模块状态错误，需要短冷却快速恢复。 | 保持 `SIM_LONG_NO_CARD_INTERVAL=8`；保持 `SIM_CFUN_COOLDOWN_SECONDS=300` 给长期无卡；`sim_error` 单独用 `SIM_CFUN_SIM_ERROR_COOLDOWN_SECONDS=30`。 |
| 成功后清理 | 如果 PPP 上线后不清恢复戳，下一次人工拔插会被上一轮冷却卡住。 | PPP 成功上线后必须调用 `clear_sim_recovery_stamps`；连续测试前可检查 `ls -l /var/run/4g-ppp*.last`。 |
| 状态显示 | `ppp0 还没有本地 IP，不能添加默认路由` 只是结果提示，不是根因；根因要看 SIM 状态和 PPP 协商阶段。 | 看到这行时继续向前找 `SIM 返回错误状态`、`CFUN`、`READY`、`CHAP/IPCP`，不要把它当成路由 bug。 |
| 防止再犯 | 这类问题跨硬件原理图、AT 状态机、PPP、路由和 UI 状态显示，不能只修一个症状。 | 修改后必须同时跑 `sh -n 22_4g_ppp/4g-ppp`、`sh 22_4g_ppp/test_board_time_sync.sh`、板端 `sha256sum /usr/bin/4g-ppp`、`cat /var/run/4g-ppp.state`、`tail -n 120 /var/log/4g-ppp.log`。 |

## 本次高德 IP 省份定位经验总结

| 经验项 | 结论 | 下次必须怎么做 |
|---|---|---|
| 室内 GPS 可靠性 | 4G 模块即使支持 GNSS，室内比赛环境也可能长时间没有有效卫星和经纬度，不能把 GPS 当成必达能力。 | 比赛现场需要稳定显示地点时，不再走 `AT+QGPS` 或 `AT+QGPSLOC`，避免界面一直卡在“定位中”。 |
| AT 串口占用 | 旧定位方案会打开 AT 管理口，容易和 `4g-ppp monitor` 的 SIM 热插拔查询抢同一个 ttyUSB。 | `4g-location` 定位脚本只走 HTTPS 高德 IP 定位接口，不访问 `/dev/ttyUSB*`，不影响 PPP 拨号和 SIM 恢复状态机。 |
| IP 定位精度 | 高德 IP 定位反映的是运营商出口公网 IP，不等于板子真实经纬度；城市/区县可能被运营商出口误导。 | 当前 UI 只显示省份，例如 `河南省`；`city/district/longitude/latitude/satellites` 保持为空是设计，不是漏填。 |
| 高德 Key 管理 | Web 服务 Key 属于板端本地私有配置，提交到 Git 会泄露。 | 只通过 `AMAP_WEB_KEY` 或 `/etc/4g-location/amap-web-key` 读取 Key；配置文件权限设为 `600`，README 只写占位符。 |
| Qt 调用节奏 | 定位是辅助显示，不应周期性打扰 4G 网络或地图配额。 | Qt 开机第一次健康刷新调用 `4g-location once` 主动定位；后续刷新只调用 `4g-location status` 读取 `/var/run/4g-location.state` 缓存。 |
| 失败排查顺序 | `state=no_key` 是缺 Key，`state=ip_failed` 是高德接口、网络或配额问题，不是 GPS 或 AT 口问题。 | 排查时先看 `cat /var/run/4g-location.state` 和 `tail -n 80 /var/log/4g-location.log`，再查 `curl -I https://restapi.amap.com` 与 4G 外网连通。 |
