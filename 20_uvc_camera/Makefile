# 交叉编译器前缀：默认使用 ST OpenSTLinux SDK 中已经安装好的 ARM 编译器。
CROSS_COMPILE ?= /opt/st/stm32mp1/3.1-snapshot/sysroots/x86_64-ostl_sdk-linux/usr/bin/arm-ostl-linux-gnueabi/arm-ostl-linux-gnueabi-

# SYSROOT 是 ST SDK 提供的 ARM 目标根目录，里面包含 errno.h、linux/videodev2.h 等目标头文件和运行库。
SYSROOT ?= /opt/st/stm32mp1/3.1-snapshot/sysroots/cortexa7t2hf-neon-vfpv4-ostl-linux-gnueabi

# CC 是最终使用的 C 编译器；GNU make 默认内置 CC=cc，所以只在默认值场景下改成 ARM 交叉编译器。
ifeq ($(origin CC),default)
CC := $(CROSS_COMPILE)gcc
endif

# TARGET 是生成给开发板运行的可执行文件名称。
TARGET := uvc_fb_preview

# SRC 是本示例唯一的源文件，内部直接使用 V4L2 和 framebuffer 系统调用。
SRC := uvc_fb_preview.c

# CFLAGS 保持偏严格，并显式指定 sysroot 与 STM32MP157 Cortex-A7 hard-float ABI。
CFLAGS ?= --sysroot=$(SYSROOT) -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -mthumb -Wall -Wextra -O2

# LDFLAGS 同样指定 sysroot，确保链接阶段使用 ARM 目标库而不是虚拟机主机库。
LDFLAGS ?= --sysroot=$(SYSROOT)

# DEPLOY_DIR 指向 NFS 根文件系统的 /tmp，作为普通用户可写的临时暂存区；正式开机自启需再运行 install_uvc_autostart.sh 复制到 /root。
DEPLOY_DIR ?= /home/cfr/linux/nfs/rootfs/tmp

.PHONY: all clean deploy

# 默认目标：编译可在 STM32MP157 Linux 用户态运行的预览程序。
all: $(TARGET)

# 编译规则：只依赖标准 Linux 头文件，不依赖 GStreamer、Qt 或 libv4l。
$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

# 部署规则：把程序复制到 NFS rootfs 的 /tmp 暂存；开发板启动后 /tmp 会被 tmpfs 覆盖，正式使用需执行安装脚本。
deploy: $(TARGET)
	cp $(TARGET) $(DEPLOY_DIR)/$(TARGET)
	chmod 755 $(DEPLOY_DIR)/$(TARGET)

# 清理规则：删除本目录生成的可执行文件。
clean:
	rm -f $(TARGET)
