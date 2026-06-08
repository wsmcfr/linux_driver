#!/bin/sh

# STM32MP157 登录环境时区配置脚本。
# 该文件部署到 /etc/profile.d/board-timezone.sh 后，会在用户登录 shell 时被 /etc/profile 自动加载。
# 作用是让人工输入裸 `date` 时默认显示北京时间，而不是 BusyBox 默认的 UTC。

# 保存板端业务时区。
# POSIX TZ 规则里 CST-8 表示 UTC+8，北京时间正好使用这个偏移。
BOARD_TIME_ZONE="${BOARD_TIME_ZONE:-CST-8}"

# 导出 TZ 到当前登录 shell。
# 这样 root 登录后直接执行 date 也会显示 CST 时间；脚本和服务仍可根据需要覆盖 TZ。
export TZ="$BOARD_TIME_ZONE"
