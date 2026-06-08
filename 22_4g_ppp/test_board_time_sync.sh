#!/bin/sh

# 测试目的：
#   验证 4G PPP 模块已经接入开机联网后的北京时间同步流程。
#
# 主要流程：
#   1. 定位当前模块目录。
#   2. 检查板端校时脚本是否存在，并包含 NTP、时区和 RTC 写回逻辑。
#   3. 检查 4G PPP 管理脚本是否在联网成功后调用校时脚本。
#   4. 检查模块 README 是否记录部署、验证和失败排查方法。
#
# 关键参数：
#   本脚本不接收外部参数，必须在仓库任意目录下都可运行。
#
# 返回值：
#   0 表示契约满足；非 0 表示缺少脚本、调用点或文档记录。

set -eu

# 保存脚本所在目录，保证从仓库根目录或模块目录执行时都能找到待测文件。
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# 保存待测校时脚本路径；后续部署到板端时应安装到 /usr/bin/board-time-sync。
TIME_SYNC_SCRIPT="$SCRIPT_DIR/board-time-sync"

# 保存待测 4G 管理脚本路径；后续部署到板端时应安装到 /usr/bin/4g-ppp。
PPP_SCRIPT="$SCRIPT_DIR/4g-ppp"

# 保存待测登录时区脚本路径；后续部署到板端时应安装到 /etc/profile.d/board-timezone.sh。
PROFILE_TZ_SCRIPT="$SCRIPT_DIR/board-timezone.sh"

# 保存模块文档路径，用于检查是否同步记录使用方法和验证命令。
README_FILE="$SCRIPT_DIR/README.md"

# 判断文件是否存在；缺文件时直接报错并退出，避免后续 grep 输出误导。
require_file()
{
	# 参数 $1 表示必须存在的文件路径。
	if [ ! -f "$1" ]; then
		printf 'FAIL: missing file: %s\n' "$1" >&2
		exit 1
	fi
}

# 判断文件内容是否包含指定固定字符串；使用 grep -F 避免正则转义差异。
require_text()
{
	# 参数 $1 表示被检查文件，参数 $2 表示必须出现的固定文本。
	if ! grep -Fq "$2" "$1"; then
		printf 'FAIL: %s missing text: %s\n' "$1" "$2" >&2
		exit 1
	fi
}

require_file "$TIME_SYNC_SCRIPT"
require_file "$PPP_SCRIPT"
require_file "$PROFILE_TZ_SCRIPT"
require_file "$README_FILE"

# 校时脚本必须显式维护北京时间时区，避免系统时间同步后又回到 UTC 显示。
require_text "$TIME_SYNC_SCRIPT" 'CST-8'
require_text "$TIME_SYNC_SCRIPT" 'ntpd -n -q -p'
require_text "$TIME_SYNC_SCRIPT" 'hwclock -w -u'
require_text "$TIME_SYNC_SCRIPT" '/var/log/board-time-sync.log'
require_text "$TIME_SYNC_SCRIPT" 'HTTP_TIME_URLS="${HTTP_TIME_URLS:-http://139.9.35.72/health https://cloud.tencent.com}"'

# 4G 管理脚本必须在 PPP 联网成功和默认路由处理完成后调用校时脚本。
require_text "$PPP_SCRIPT" 'TIME_SYNC_CMD="${TIME_SYNC_CMD:-/usr/bin/board-time-sync}"'
require_text "$PPP_SCRIPT" 'BOARD_TIME_ZONE="${BOARD_TIME_ZONE:-CST-8}"'
require_text "$PPP_SCRIPT" 'export TZ="$BOARD_TIME_ZONE"'
require_text "$PPP_SCRIPT" 'sync_board_time'
require_text "$PPP_SCRIPT" '"$TIME_SYNC_CMD" once'
require_text "$PPP_SCRIPT" 'USB_POWER_CONTROLS="${USB_POWER_CONTROLS:-/sys/bus/usb/devices/2-1/power/control /sys/bus/usb/devices/2-1.7/power/control}"'
require_text "$PPP_SCRIPT" 'ensure_usb_runtime_power'
require_text "$PPP_SCRIPT" 'echo on > "$control"'

# 4G 管理脚本必须具备 SIM 热插拔恢复状态机；当前原理图没有把 USIM_PRESENT 接到卡座检测脚，
# 因此脚本必须用“无卡低频轮询 + 串口 URC 被动唤醒 + 插回短时快速确认”兜底。
require_text "$PPP_SCRIPT" 'STATE_FILE="${STATE_FILE:-/var/run/4g-ppp.state}"'
require_text "$PPP_SCRIPT" 'MONITOR_PID_FILE="${MONITOR_PID_FILE:-/var/run/4g-ppp-monitor.pid}"'
require_text "$PPP_SCRIPT" 'AT_TTY_CANDIDATES="${AT_TTY_CANDIDATES:-$PPP_TTY /dev/ttyUSB2 /dev/ttyUSB1 /dev/ttyUSB0 /dev/ttyUSB3}"'
require_text "$PPP_SCRIPT" 'SIM_RECENT_NO_CARD_INTERVAL="${SIM_RECENT_NO_CARD_INTERVAL:-5}"'
require_text "$PPP_SCRIPT" 'SIM_RECENT_NO_CARD_WINDOW_SECONDS="${SIM_RECENT_NO_CARD_WINDOW_SECONDS:-8}"'
require_text "$PPP_SCRIPT" 'SIM_LONG_NO_CARD_INTERVAL="${SIM_LONG_NO_CARD_INTERVAL:-8}"'
require_text "$PPP_SCRIPT" 'SIM_FAST_RETRY_SECONDS="${SIM_FAST_RETRY_SECONDS:-1}"'
require_text "$PPP_SCRIPT" 'SIM_FAST_WAIT_SECONDS="${SIM_FAST_WAIT_SECONDS:-30}"'
require_text "$PPP_SCRIPT" 'SIM_ONLINE_SIM_CHECK_INTERVAL="${SIM_ONLINE_SIM_CHECK_INTERVAL:-30}"'
require_text "$PPP_SCRIPT" 'SIM_ONLINE_PING_TIMEOUT="${SIM_ONLINE_PING_TIMEOUT:-5}"'
require_text "$PPP_SCRIPT" 'SIM_UNKNOWN_INTERVAL="${SIM_UNKNOWN_INTERVAL:-8}"'
require_text "$PPP_SCRIPT" 'SIM_ERROR_FAST_WAIT_SECONDS="${SIM_ERROR_FAST_WAIT_SECONDS:-10}"'
require_text "$PPP_SCRIPT" 'SIM_ERROR_FAST_COOLDOWN_SECONDS="${SIM_ERROR_FAST_COOLDOWN_SECONDS:-45}"'
require_text "$PPP_SCRIPT" 'SIM_CFUN_SIM_ERROR_COOLDOWN_SECONDS="${SIM_CFUN_SIM_ERROR_COOLDOWN_SECONDS:-30}"'
require_text "$PPP_SCRIPT" 'SIM_AFTER_CFUN_READY_WAIT_SECONDS="${SIM_AFTER_CFUN_READY_WAIT_SECONDS:-45}"'
require_text "$PPP_SCRIPT" 'SIM_PRESENT_WIRED="${SIM_PRESENT_WIRED:-0}"'
require_text "$PPP_SCRIPT" 'write_state'
require_text "$PPP_SCRIPT" 'find_at_tty'
require_text "$PPP_SCRIPT" 'at_exchange_on_tty'
require_text "$PPP_SCRIPT" 'at_tty=%s'
require_text "$PPP_SCRIPT" 'query_sim_status'
require_text "$PPP_SCRIPT" '+CME ERROR: *13'
require_text "$PPP_SCRIPT" 'online_sim_check'
require_text "$PPP_SCRIPT" 'online_link_check'
require_text "$PPP_SCRIPT" 'ping -I "$PPP_IF"'
require_text "$PPP_SCRIPT" 'wait_for_sim_ready'
require_text "$PPP_SCRIPT" 'wait_for_sim_urc_or_timeout'
require_text "$PPP_SCRIPT" 'try_sim_error_fast_ready'
require_text "$PPP_SCRIPT" 'clear_sim_recovery_stamps'
require_text "$PPP_SCRIPT" 'monitor_loop'
require_text "$PPP_SCRIPT" 'start_with_monitor'
require_text "$PPP_SCRIPT" 'sim-status)'
require_text "$PPP_SCRIPT" 'monitor-start)'
require_text "$PPP_SCRIPT" 'AT+CPIN?'
require_text "$PPP_SCRIPT" 'AT+QSIMSTAT=1'
require_text "$PPP_SCRIPT" 'AT+QSIMDET=1,0'

# 登录环境必须显式导出 TZ，否则 root 登录后裸 date 仍会显示 UTC。
require_text "$PROFILE_TZ_SCRIPT" 'BOARD_TIME_ZONE="${BOARD_TIME_ZONE:-CST-8}"'
require_text "$PROFILE_TZ_SCRIPT" 'export TZ="$BOARD_TIME_ZONE"'

# 文档必须记录校时部署、手动验证和失败排查，避免只改脚本不留操作入口。
require_text "$README_FILE" 'board-time-sync once'
require_text "$README_FILE" '/etc/profile.d/board-timezone.sh'
require_text "$README_FILE" 'hwclock -r'
require_text "$README_FILE" '/var/log/board-time-sync.log'
require_text "$README_FILE" 'power/control'
require_text "$README_FILE" 'usb 2-1.7: USB disconnect'
require_text "$README_FILE" 'USIM_PRESENT'
require_text "$README_FILE" '没有接到 Nano SIM 卡座检测脚'
require_text "$README_FILE" '4g-ppp monitor-start'
require_text "$README_FILE" '4g-ppp sim-status'
require_text "$README_FILE" 'S80ppp-4g start'
require_text "$README_FILE" 'SIM_LONG_NO_CARD_INTERVAL'
require_text "$README_FILE" '无卡低频轮询'

printf 'PASS: 4G PPP board time sync contract\n'
