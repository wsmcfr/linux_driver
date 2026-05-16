# 指定内核源码目录，外部模块需要进入这个内核目录调用 Kbuild 编译。
KERNELDIR := /home/cfr/linux/atk-mp1/linux/my_linux/linux-5.4.31
# 记录当前驱动目录路径，Kbuild 通过 M=$(CURRENT_PATH) 回到这里编译模块源码。
CURRENT_PATH := $(shell pwd)
# 指定默认交叉编译工具链前缀，用户也可以在命令行覆盖这个变量。
CROSS_COMPILE ?= arm-none-linux-gnueabihf-
# 指定用户态测试程序编译器，默认使用 ARM Linux 交叉编译 gcc。
APP_CC ?= $(CROSS_COMPILE)gcc
# 指定最终生成的用户态触摸测试程序名称。
APP := iictouchapp

# 声明需要编译成内核模块的目标文件，Kbuild 会由 iictouch.o 生成 iictouch.ko。
obj-m := iictouch.o

# 声明这些目标不是实际文件名，避免同名文件影响 make 执行。
.PHONY: build kernel_modules app clean

# 默认构建目标：先编译内核模块，再编译用户态测试程序。
build: kernel_modules app

# kernel_modules 目标负责调用内核 Kbuild 编译 iictouch.ko。
kernel_modules:
	$(MAKE) -C $(KERNELDIR) M=$(CURRENT_PATH) modules

# app 目标负责交叉编译用户态触摸测试程序 iictouchapp。
app:
	$(APP_CC) -Wall -O2 iictouchapp.c -o $(APP)

# clean 目标负责清理内核模块编译产物和用户态测试程序。
clean:
	$(MAKE) -C $(KERNELDIR) M=$(CURRENT_PATH) clean
	$(RM) $(APP)
