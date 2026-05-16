# 指定默认交叉编译工具链前缀，适配正点原子 STM32MP157 Linux 开发环境。
CROSS_COMPILE ?= arm-none-linux-gnueabihf-

# 指定目标板用户态程序编译器，用户可以在命令行通过 APP_CC=xxx 覆盖。
APP_CC ?= $(CROSS_COMPILE)gcc

# 指定宿主机测试编译器，进入虚拟机后可用 make host 做语法检查。
HOST_CC ?= gcc

# 指定通用编译选项，打开常见告警并做 O2 优化。
CFLAGS ?= -Wall -Wextra -O2

# 指定最终生成的 STM32MP157 串口测试程序名称。
APP := uartapp

# 声明这些目标不是实际文件名，避免同名文件影响 make 执行。
.PHONY: all app host clean

# 默认目标编译适合拷贝到 STM32MP157 开发板运行的 ARM Linux 程序。
all: app

# app 目标负责使用交叉编译器生成目标板程序。
app: uartapp.c
	$(APP_CC) $(CFLAGS) uartapp.c -o $(APP)

# host 目标负责在 Linux 虚拟机本机编译一份检查语法，不用于拷贝到开发板。
host: uartapp.c
	$(HOST_CC) $(CFLAGS) uartapp.c -o $(APP).host

# clean 目标负责删除编译生成的程序，保持源码目录干净。
clean:
	$(RM) $(APP) $(APP).host
