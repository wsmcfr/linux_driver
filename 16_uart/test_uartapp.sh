#!/bin/sh
# 这个脚本用于验证 uartapp 的基础命令行行为和伪串口收发能力。
# 它使用 Linux PTY 模拟串口两端，因此不需要真实 STM32MP157 或 STM32F4 硬件也能先跑基础测试。

set -eu

# SCRIPT_DIR 保存脚本所在目录，保证从仓库根目录或当前目录执行都能找到 uartapp.c。
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# APP_SRC 保存待测试的 C 源文件路径。
APP_SRC="${SCRIPT_DIR}/uartapp.c"

# APP_BIN 保存测试编译生成的宿主机临时可执行文件。
APP_BIN="${SCRIPT_DIR}/uartapp.host"

# cleanup 用于删除测试临时产物，避免把宿主机二进制留在源码目录。
cleanup()
{
	rm -f "${APP_BIN}"
}

# 注册退出清理函数，正常成功和异常失败都会执行。
trap cleanup EXIT

# 使用宿主机 gcc 严格编译一次，主要检查 Linux 头文件、语法和常见告警。
"${HOST_CC:-gcc}" -Wall -Wextra -Werror -O2 "${APP_SRC}" -o "${APP_BIN}"

# -h 应该返回成功，并输出 Usage，说明用户可以直接查看命令格式。
"${APP_BIN}" -h | grep -q "Usage:"

# 缺少参数应该返回失败，避免程序误打开错误设备或静默成功。
if "${APP_BIN}" >/tmp/uartapp-noargs.out 2>/tmp/uartapp-noargs.err; then
	echo "uartapp without arguments should fail" >&2
	exit 1
fi

# 不支持的波特率应该返回失败，避免错误配置进入真实串口链路。
if "${APP_BIN}" /dev/null 12345 recv >/tmp/uartapp-badbaud.out 2>/tmp/uartapp-badbaud.err; then
	echo "uartapp with unsupported baud should fail" >&2
	exit 1
fi

# 使用 Python 创建 PTY 伪串口，验证 send 发出的真实字节和 recv 打印的接收字节。
python3 - "${APP_BIN}" <<'PY'
import os
import pty
import signal
import subprocess
import sys
import time

# app_bin 保存 shell 传进来的 uartapp.host 路径。
app_bin = sys.argv[1]

# master/slave 组成一对伪串口：uartapp 打开 slave，测试脚本从 master 侧读写。
master, slave = pty.openpty()
slave_name = os.ttyname(slave)

try:
    # 验证 send 模式：uartapp 往 slave 写，测试脚本必须能在 master 侧读到 hello\r\n。
    proc = subprocess.run(
        [app_bin, slave_name, "115200", "send", "hello\\r\\n"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=5,
    )
    data = os.read(master, 64)
    assert proc.returncode == 0, proc.stderr.decode(errors="replace")
    assert data == b"hello\r\n", repr(data)

    # 验证 recv 模式：测试脚本从 master 写入 F4_OK\r\n，uartapp 必须打印对应 HEX。
    proc = subprocess.Popen(
        [app_bin, slave_name, "115200", "recv"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        universal_newlines=True,
    )
    time.sleep(0.3)
    os.write(master, b"F4_OK\r\n")
    time.sleep(0.3)
    proc.send_signal(signal.SIGINT)
    out, err = proc.communicate(timeout=3)
    assert "46 34 5F 4F 4B 0D 0A" in out, out + err
finally:
    # 测试结束后关闭 PTY 两端，避免文件描述符泄漏。
    os.close(master)
    os.close(slave)
PY

# 走到这里表示编译、参数校验、伪串口发送和伪串口接收都通过。
echo "uartapp tests passed"
