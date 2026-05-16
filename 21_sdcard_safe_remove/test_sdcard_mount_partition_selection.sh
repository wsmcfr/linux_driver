#!/bin/sh
#
# 作用：
#   回归测试 S85sdcard-mount 的外置 SD 分区选择逻辑。
#   重点覆盖“格式化后 FAT32 数据分区不在 p1，而在 p3”的真实板端场景。
#
# 主要流程：
#   1. 创建临时 fakebin，把 cat/blkid/lsblk/findmnt 等命令替换为可控桩程序。
#   2. 通过 TEST_SDCARD_SYS_BLOCK_DIR 和 TEST_SDCARD_DEV_DIR 注入虚拟 mmcblk 设备。
#   3. 调用脚本内部的 find_sd_partition，检查它是否选择真正的 vfat 分区。
#
# 返回值：
#   全部用例通过返回 0；任一断言失败返回 1。

set -eu

# SCRIPT_DIR 表示本测试脚本所在目录，用于定位被测的 S85sdcard-mount。
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"

# TARGET_SCRIPT 是被测挂载服务源码。
TARGET_SCRIPT="$SCRIPT_DIR/S85sdcard-mount"

# TMP_DIR 保存本次测试的假 sysfs、假 dev 和命令桩，退出时自动删除。
TMP_DIR="$(mktemp -d)"

# 清理临时目录，避免测试失败后残留假设备文件影响下一次运行。
cleanup()
{
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT

# FAKE_BIN 放置命令桩。把它放到 PATH 最前面后，被测脚本会优先调用这里的 cat/blkid。
FAKE_BIN="$TMP_DIR/fakebin"

# FAKE_SYS_BLOCK 模拟 /sys/block。
FAKE_SYS_BLOCK="$TMP_DIR/sys/block"

# FAKE_DEV 模拟 /dev。
FAKE_DEV="$TMP_DIR/dev"

mkdir -p "$FAKE_BIN" "$FAKE_SYS_BLOCK" "$FAKE_DEV"

cat > "$FAKE_BIN/cat" <<'FAKE_CAT'
#!/bin/sh
#
# 作用：
#   为测试拦截被测脚本读取 /sys/block/mmcblk*/device/type 的行为。

case "$1" in
    "$TEST_SDCARD_SYS_BLOCK_DIR"/mmcblk0/device/type)
        echo SD
        ;;
    "$TEST_SDCARD_SYS_BLOCK_DIR"/mmcblk1/device/type)
        echo MMC
        ;;
    *)
        exec /bin/cat "$@"
        ;;
esac
FAKE_CAT

cat > "$FAKE_BIN/blkid" <<'FAKE_BLKID'
#!/bin/sh
#
# 作用：
#   为测试提供可控的文件系统类型识别结果。

case "$1" in
    "$TEST_SDCARD_DEV_DIR"/mmcblk0p1)
        echo "$1: PARTLABEL=\"loader\""
        ;;
    "$TEST_SDCARD_DEV_DIR"/mmcblk0p2)
        echo "$1: PARTLABEL=\"boot\""
        ;;
    "$TEST_SDCARD_DEV_DIR"/mmcblk0p3)
        echo "$1: LABEL=\"K230\" TYPE=\"vfat\""
        ;;
    "$TEST_SDCARD_DEV_DIR"/mmcblk1p1)
        echo "$1: TYPE=\"vfat\""
        ;;
    *)
        exit 2
        ;;
esac
FAKE_BLKID

cat > "$FAKE_BIN/fdisk" <<'FAKE_FDISK'
#!/bin/sh
#
# 作用：
#   为测试提供可控的分区表类型输出，模拟 BusyBox blkid 不输出 TYPE 的板端场景。

case "$2" in
    "$TEST_SDCARD_DEV_DIR"/mmcblk2)
        cat <<EOF
Disk $2: 29 GB

Device       Boot StartCHS    EndCHS        StartLBA     EndLBA    Sectors  Size Id Type
$TEST_SDCARD_DEV_DIR/mmcblk2p1    0,32,33     1023,254,63       2048   61028351   61026304 29.0G  c Win95 FAT32 (LBA)
EOF
        ;;
    *)
        exit 1
        ;;
esac
FAKE_FDISK

chmod 755 "$FAKE_BIN/cat" "$FAKE_BIN/blkid" "$FAKE_BIN/fdisk"

mkdir -p "$FAKE_SYS_BLOCK/mmcblk0/device" "$FAKE_SYS_BLOCK/mmcblk1/device"
: > "$FAKE_DEV/mmcblk0p1"
: > "$FAKE_DEV/mmcblk0p2"
: > "$FAKE_DEV/mmcblk0p3"
: > "$FAKE_DEV/mmcblk1p1"

run_find_sd_partition()
{
    # run_find_sd_partition 的作用：
    #   在受控环境中加载被测脚本，只执行 find_sd_partition 函数。
    #
    # 参数：
    #   无显式参数；通过环境变量注入测试路径。
    #
    # 返回值：
    #   stdout 输出 find_sd_partition 返回的分区路径。
    env \
        PATH="$FAKE_BIN:/sbin:/bin:/usr/sbin:/usr/bin" \
        TEST_SDCARD_PATH="$FAKE_BIN:/sbin:/bin:/usr/sbin:/usr/bin" \
        TEST_SDCARD_SYS_BLOCK_DIR="$FAKE_SYS_BLOCK" \
        TEST_SDCARD_DEV_DIR="$FAKE_DEV" \
        TEST_SDCARD_MOUNT_SOURCE_ONLY=1 \
        sh -c '. "$0"; find_sd_partition' "$TARGET_SCRIPT"
}

# actual 保存被测函数选出的分区路径。
actual="$(run_find_sd_partition)"

# expected 是本次测试期望选中的真实 FAT32 数据分区。
expected="$FAKE_DEV/mmcblk0p3"

if [ "$actual" != "$expected" ]; then
    echo "FAIL: expected $expected, got $actual" >&2
    exit 1
fi

echo "PASS: FAT partition selection prefers real vfat partition"

# 重新布置第二组 fake 设备：
#   mmcblk2 是外置 SD，只有 p1；
#   blkid 只输出 LABEL/UUID，不输出 TYPE；
#   fdisk 显示 p1 是 Win95 FAT32 (LBA)。
# 这对应 2026-05-16 板端格式化后的真实现象。
rm -rf "$FAKE_SYS_BLOCK" "$FAKE_DEV"
mkdir -p "$FAKE_SYS_BLOCK/mmcblk2/device" "$FAKE_DEV"
: > "$FAKE_DEV/mmcblk2p1"

cat > "$FAKE_BIN/cat" <<'FAKE_CAT_LABEL_ONLY'
#!/bin/sh
#
# 作用：
#   为第二组测试提供外置 SD 的 sysfs type。

case "$1" in
    "$TEST_SDCARD_SYS_BLOCK_DIR"/mmcblk2/device/type)
        echo SD
        ;;
    *)
        exec /bin/cat "$@"
        ;;
esac
FAKE_CAT_LABEL_ONLY

cat > "$FAKE_BIN/blkid" <<'FAKE_BLKID_LABEL_ONLY'
#!/bin/sh
#
# 作用：
#   模拟板端 blkid 只输出 LABEL/UUID，不输出 TYPE 的情况。

case "$1" in
    "$TEST_SDCARD_DEV_DIR"/mmcblk2p1)
        echo "$1: LABEL=\"新加卷\" UUID=\"1E08-F7CA\""
        ;;
    *)
        exit 2
        ;;
esac
FAKE_BLKID_LABEL_ONLY

chmod 755 "$FAKE_BIN/cat" "$FAKE_BIN/blkid"

actual="$(run_find_sd_partition)"
expected="$FAKE_DEV/mmcblk2p1"

if [ "$actual" != "$expected" ]; then
    echo "FAIL: expected fdisk fallback $expected, got $actual" >&2
    exit 1
fi

echo "PASS: FAT partition selection falls back to fdisk type"
