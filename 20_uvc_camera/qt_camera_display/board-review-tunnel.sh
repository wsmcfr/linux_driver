#!/bin/sh
#
# 作用：
#   在 STM32MP157 板端长期维护一条到云服务器的反向 SSH 隧道。
#   云端后端访问 http://127.0.0.1:18081/api/v1/review-result 时，
#   sshd 会把请求转发到板端本机 http://127.0.0.1:18080/api/v1/review-result。
#
# 运行形态：
#   1. 板端开机 init 脚本调用本脚本 start。
#   2. start 只启动一个后台 monitor_loop 守护进程。
#   3. monitor_loop 周期检查 ssh 隧道进程是否还存在。
#   4. ssh 断开、云端重启、网络抖动后，monitor_loop 会重新拉起 ssh -R。
#
# 注意：
#   最终现场运行时不依赖 Windows 或虚拟机；虚拟机只用于部署和调试。

# set -u 用于发现变量拼写错误；不使用 set -e，避免一次检测失败导致守护进程退出。
set -u

# CLOUD_USER 是云服务器上用于承载受限反向隧道的 Linux 用户。
CLOUD_USER="${CLOUD_USER:-ubuntu}"

# CLOUD_HOST 是云服务器公网 IP 或域名；板端主动连接它。
CLOUD_HOST="${CLOUD_HOST:-139.9.35.72}"

# REMOTE_PORT 是云端本机回环监听端口，云端后端设备配置应访问 127.0.0.1:18081。
REMOTE_PORT="${REMOTE_PORT:-18081}"

# LOCAL_HOST 是板端本地 Qt 回写服务地址；保持 127.0.0.1 可避免把板端服务暴露到局域网。
LOCAL_HOST="${LOCAL_HOST:-127.0.0.1}"

# LOCAL_PORT 是板端 Qt 程序内置的云端复核回写 HTTP 服务端口。
LOCAL_PORT="${LOCAL_PORT:-18080}"

# KEY_FILE 是板端专用隧道私钥；云端 authorized_keys 已限制它只允许指定反向转发。
KEY_FILE="${KEY_FILE:-/root/.ssh/id_ed25519_yunfuwu_tunnel}"

# CHECK_INTERVAL 是守护循环间隔，单位秒；断线后最多约一个周期内重新尝试。
CHECK_INTERVAL="${CHECK_INTERVAL:-20}"

# LOG_FILE 记录板端守护动作；放 /tmp 避免频繁写入 SD 卡或只读 rootfs。
LOG_FILE="${LOG_FILE:-/tmp/board-review-tunnel.log}"

# PID_DIR 保存守护进程和 ssh 子进程 PID；/var/run 不可用时可通过环境变量改到 /tmp。
PID_DIR="${PID_DIR:-/var/run}"

# MONITOR_PID_FILE 保存 monitor_loop 守护进程 PID，用于 start/status/stop 判断。
MONITOR_PID_FILE="${MONITOR_PID_FILE:-$PID_DIR/board-review-tunnel.pid}"

# SSH_PID_FILE 保存当前 ssh -R 进程 PID，用于 stop 时优先精准终止。
SSH_PID_FILE="${SSH_PID_FILE:-$PID_DIR/board-review-tunnel-ssh.pid}"

# LOCAL_CHECK_URL 是本地回写服务探测地址；GET 返回 404 也说明 Qt HTTP 服务已响应。
LOCAL_CHECK_URL="${LOCAL_CHECK_URL:-http://${LOCAL_HOST}:${LOCAL_PORT}/api/v1/review-result}"

# REMOTE_FORWARD_SPEC 是传给 ssh -R 的完整反向转发规格。
REMOTE_FORWARD_SPEC="127.0.0.1:${REMOTE_PORT}:${LOCAL_HOST}:${LOCAL_PORT}"

# SSH_DEST 是 ssh 登录目标；不执行远程命令，只建立端口转发。
SSH_DEST="${CLOUD_USER}@${CLOUD_HOST}"

ensure_runtime_dir()
{
    # ensure_runtime_dir 的作用：
    #   确保 PID 文件目录存在，避免 Buildroot 精简系统开机时 /var/run 尚未创建。
    # 主要流程：
    #   1. 创建 PID_DIR。
    #   2. 如果创建失败，则把 PID 文件降级到 /tmp。
    # 参数：
    #   无。
    # 返回值：
    #   始终返回 0；失败时使用 /tmp 兜底。
    if ! mkdir -p "$PID_DIR" 2>/dev/null; then
        PID_DIR="/tmp"
        MONITOR_PID_FILE="$PID_DIR/board-review-tunnel.pid"
        SSH_PID_FILE="$PID_DIR/board-review-tunnel-ssh.pid"
        mkdir -p "$PID_DIR" 2>/dev/null || true
    fi
}

log_msg()
{
    # log_msg 的作用：
    #   给守护脚本动作补充带时间戳的日志，便于现场只看 /tmp 日志定位问题。
    # 主要流程：
    #   1. 生成当前时间。
    #   2. 把调用者传入的消息追加到 LOG_FILE。
    # 参数：
    #   $* 是要写入日志的一整行文本。
    # 返回值：
    #   日志写入成功返回 0；失败不影响主流程。
    now="$(date '+%Y-%m-%d %H:%M:%S' 2>/dev/null || date)"
    printf '%s %s\n' "$now" "$*" >> "$LOG_FILE" 2>/dev/null || true
}

rotate_log_if_needed()
{
    # rotate_log_if_needed 的作用：
    #   限制 /tmp 日志大小，避免长时间运行把 tmpfs 写满。
    # 主要流程：
    #   1. 读取 LOG_FILE 当前字节数。
    #   2. 超过 256KB 时把旧日志改名为 .1，并重新开始写新日志。
    # 参数：
    #   无。
    # 返回值：
    #   始终返回 0。
    if [ -f "$LOG_FILE" ]; then
        log_size="$(wc -c < "$LOG_FILE" 2>/dev/null || echo 0)"
        if [ "$log_size" -gt 262144 ] 2>/dev/null; then
            mv "$LOG_FILE" "$LOG_FILE.1" 2>/dev/null || true
        fi
    fi
}

pid_is_alive()
{
    # pid_is_alive 的作用：
    #   判断一个 PID 当前是否仍在系统进程表中。
    # 主要流程：
    #   1. 参数为空时直接失败。
    #   2. 使用 kill -0 做无副作用探测。
    # 参数：
    #   $1 是需要检测的 PID。
    # 返回值：
    #   PID 存活返回 0；不存在或参数为空返回 1。
    test_pid="${1:-}"
    [ -n "$test_pid" ] || return 1
    kill -0 "$test_pid" 2>/dev/null
}

monitor_is_alive()
{
    # monitor_is_alive 的作用：
    #   判断后台 monitor_loop 是否已经运行，避免重复启动多个守护循环。
    # 主要流程：
    #   1. 读取 MONITOR_PID_FILE。
    #   2. 用 pid_is_alive 确认该 PID 是否仍存在。
    # 参数：
    #   无。
    # 返回值：
    #   守护进程存活返回 0；否则返回 1。
    [ -f "$MONITOR_PID_FILE" ] || return 1
    monitor_pid="$(cat "$MONITOR_PID_FILE" 2>/dev/null || true)"
    pid_is_alive "$monitor_pid"
}

find_tunnel_pids()
{
    # find_tunnel_pids 的作用：
    #   从进程表中查找当前这条反向隧道对应的 ssh 进程。
    # 主要流程：
    #   1. 使用 ps w 获取较完整命令行，避免 BusyBox ps 默认截断太短。
    #   2. 同时匹配 ssh、-R 和 REMOTE_FORWARD_SPEC，降低误杀其它 SSH 会话的风险。
    # 参数：
    #   无。
    # 返回值：
    #   stdout 输出匹配到的 PID；没有匹配时输出为空。
    ps w 2>/dev/null \
        | grep '[s]sh' \
        | grep -- "-R" \
        | grep -- "$REMOTE_FORWARD_SPEC" \
        | awk '{print $1}'
}

remember_ssh_pid()
{
    # remember_ssh_pid 的作用：
    #   把当前隧道 ssh PID 写入文件，便于 status 和 stop 精准处理。
    # 主要流程：
    #   1. 调用 find_tunnel_pids。
    #   2. 找到 PID 时写入 SSH_PID_FILE；找不到时删除旧 PID 文件。
    # 参数：
    #   无。
    # 返回值：
    #   找到 ssh 进程返回 0；未找到返回 1。
    tunnel_pids="$(find_tunnel_pids | tr '\n' ' ')"
    if [ -n "$tunnel_pids" ]; then
        printf '%s\n' "$tunnel_pids" > "$SSH_PID_FILE"
        return 0
    fi
    rm -f "$SSH_PID_FILE"
    return 1
}

ssh_tunnel_is_alive()
{
    # ssh_tunnel_is_alive 的作用：
    #   判断反向隧道 ssh 进程是否仍在运行。
    # 主要流程：
    #   1. 优先检查 SSH_PID_FILE 中记录的 PID。
    #   2. PID 文件失效时再从进程表按命令行匹配。
    # 参数：
    #   无。
    # 返回值：
    #   隧道进程存在返回 0；否则返回 1。
    if [ -f "$SSH_PID_FILE" ]; then
        for pid in $(cat "$SSH_PID_FILE" 2>/dev/null || true); do
            if pid_is_alive "$pid"; then
                return 0
            fi
        done
    fi
    remember_ssh_pid >/dev/null 2>&1
}

local_review_service_state()
{
    # local_review_service_state 的作用：
    #   探测板端 Qt 回写服务是否有 HTTP 响应，辅助判断云端 502/拒绝连接是不是板端服务未启动。
    # 主要流程：
    #   1. 用 curl 请求板端本机 review-result 地址。
    #   2. 只要得到 2xx/3xx/4xx 状态码就认为 HTTP 服务已响应。
    #   3. 连接失败、超时或没有 curl 时返回 not-ready。
    # 参数：
    #   无。
    # 返回值：
    #   stdout 输出 ready:<code> 或 not-ready:<原因>；函数返回码成功/失败与状态一致。
    if ! command -v curl >/dev/null 2>&1; then
        printf '%s\n' 'not-ready:no-curl'
        return 1
    fi

    http_code="$(curl -sS -o /dev/null -w '%{http_code}' --max-time 3 "$LOCAL_CHECK_URL" 2>/dev/null || echo 000)"
    case "$http_code" in
        2*|3*|4*)
            printf 'ready:%s\n' "$http_code"
            return 0
            ;;
        *)
            printf 'not-ready:%s\n' "$http_code"
            return 1
            ;;
    esac
}

start_ssh_tunnel()
{
    # start_ssh_tunnel 的作用：
    #   启动真正的 ssh -R 反向隧道进程。
    # 主要流程：
    #   1. 检查 ssh 命令和专用私钥是否存在。
    #   2. 使用 ExitOnForwardFailure=yes 保证云端端口绑定失败时 ssh 直接失败退出。
    #   3. 使用 ServerAliveInterval/CountMax 让网络断开后 ssh 自动退出，交给 monitor_loop 重连。
    # 参数：
    #   无。
    # 返回值：
    #   ssh 成功后台化并能找到进程返回 0；否则返回非 0。
    if ! command -v ssh >/dev/null 2>&1; then
        log_msg "ERROR ssh 命令不存在，无法建立反向隧道"
        return 1
    fi

    if [ ! -r "$KEY_FILE" ]; then
        log_msg "ERROR 隧道私钥不存在或不可读：$KEY_FILE"
        return 1
    fi

    local_state="$(local_review_service_state 2>/dev/null || true)"
    log_msg "INFO 准备启动反向隧道 remote=${REMOTE_FORWARD_SPEC} dest=${SSH_DEST} local_service=${local_state}"

    ssh -f -N -T \
        -i "$KEY_FILE" \
        -o IdentitiesOnly=yes \
        -o BatchMode=yes \
        -o StrictHostKeyChecking=accept-new \
        -o ServerAliveInterval=30 \
        -o ServerAliveCountMax=3 \
        -o ExitOnForwardFailure=yes \
        -R "$REMOTE_FORWARD_SPEC" \
        "$SSH_DEST" >> "$LOG_FILE" 2>&1
    ssh_rc=$?

    if [ "$ssh_rc" -ne 0 ]; then
        log_msg "ERROR ssh -R 启动失败 rc=${ssh_rc}；常见原因是云端 127.0.0.1:${REMOTE_PORT} 已被旧隧道占用、网络未通或 key 未授权"
        return "$ssh_rc"
    fi

    if remember_ssh_pid; then
        log_msg "INFO 反向隧道已启动 ssh_pid=$(cat "$SSH_PID_FILE" 2>/dev/null | tr '\n' ' ')"
        return 0
    fi

    log_msg "ERROR ssh 命令返回成功但未在进程表中找到隧道进程"
    return 1
}

stop_ssh_tunnel()
{
    # stop_ssh_tunnel 的作用：
    #   终止当前脚本管理的 ssh 反向隧道进程。
    # 主要流程：
    #   1. 合并 PID 文件和进程表查找结果。
    #   2. 先发送 TERM，让 ssh 正常关闭云端远端监听。
    #   3. 等待后仍存活再发送 KILL，避免残留端口占用。
    # 参数：
    #   无。
    # 返回值：
    #   始终返回 0。
    pids=""
    if [ -f "$SSH_PID_FILE" ]; then
        pids="$pids $(cat "$SSH_PID_FILE" 2>/dev/null || true)"
    fi
    pids="$pids $(find_tunnel_pids | tr '\n' ' ')"

    for pid in $pids; do
        if pid_is_alive "$pid"; then
            log_msg "INFO 停止 ssh 隧道 pid=${pid}"
            kill "$pid" 2>/dev/null || true
        fi
    done

    sleep 1

    for pid in $pids; do
        if pid_is_alive "$pid"; then
            log_msg "WARN ssh 隧道 pid=${pid} 未正常退出，发送 KILL"
            kill -9 "$pid" 2>/dev/null || true
        fi
    done

    rm -f "$SSH_PID_FILE"
    return 0
}

monitor_loop()
{
    # monitor_loop 的作用：
    #   长期守护反向隧道；这是板端独立运行时真正负责断线重连的循环。
    # 主要流程：
    #   1. 写入自身 PID。
    #   2. 周期检查 ssh 隧道进程是否存在。
    #   3. 进程不存在时启动 ssh -R。
    #   4. 收到 TERM/INT 时清理 ssh 子进程后退出。
    # 参数：
    #   无。
    # 返回值：
    #   正常不会返回；收到停止信号后返回 0。
    ensure_runtime_dir
    printf '%s\n' "$$" > "$MONITOR_PID_FILE"
    trap 'log_msg "INFO 收到停止信号，准备退出守护循环"; stop_ssh_tunnel; rm -f "$MONITOR_PID_FILE"; exit 0' INT TERM

    log_msg "INFO board-review-tunnel monitor 启动 pid=$$ check_interval=${CHECK_INTERVAL}s"

    while :; do
        rotate_log_if_needed

        if ssh_tunnel_is_alive; then
            local_state="$(local_review_service_state 2>/dev/null || true)"
            log_msg "INFO 隧道进程存活 ssh_pid=$(cat "$SSH_PID_FILE" 2>/dev/null | tr '\n' ' ') local_service=${local_state}"
        else
            log_msg "WARN 未发现反向隧道进程，准备重连"
            start_ssh_tunnel || true
        fi

        sleep "$CHECK_INTERVAL"
    done
}

start_monitor()
{
    # start_monitor 的作用：
    #   启动后台守护循环，供 init 脚本和人工命令调用。
    # 主要流程：
    #   1. 避免重复启动。
    #   2. 用 nohup 后台执行 monitor_loop。
    #   3. 短暂等待后确认 PID 文件。
    # 参数：
    #   无。
    # 返回值：
    #   启动或已运行返回 0；启动失败返回 1。
    ensure_runtime_dir
    if monitor_is_alive; then
        echo "board-review-tunnel monitor 已运行：pid=$(cat "$MONITOR_PID_FILE" 2>/dev/null)"
        return 0
    fi

    rm -f "$MONITOR_PID_FILE"
    nohup "$0" monitor >> "$LOG_FILE" 2>&1 &
    start_pid="$!"
    log_msg "INFO 已请求启动 monitor pid=${start_pid}"
    sleep 1

    if monitor_is_alive; then
        echo "board-review-tunnel monitor 已启动：pid=$(cat "$MONITOR_PID_FILE" 2>/dev/null)"
        return 0
    fi

    echo "board-review-tunnel monitor 启动失败，请查看 $LOG_FILE" >&2
    return 1
}

stop_monitor()
{
    # stop_monitor 的作用：
    #   停止守护循环和它管理的 ssh 隧道，供 init stop/restart 调用。
    # 主要流程：
    #   1. 读取并停止 monitor PID。
    #   2. 无论 monitor 是否存在，都补充停止 ssh 隧道，清理残留。
    #   3. 删除 PID 文件。
    # 参数：
    #   无。
    # 返回值：
    #   始终返回 0。
    ensure_runtime_dir
    if [ -f "$MONITOR_PID_FILE" ]; then
        monitor_pid="$(cat "$MONITOR_PID_FILE" 2>/dev/null || true)"
        if pid_is_alive "$monitor_pid"; then
            log_msg "INFO 停止 monitor pid=${monitor_pid}"
            kill "$monitor_pid" 2>/dev/null || true
            sleep 1
            if pid_is_alive "$monitor_pid"; then
                log_msg "WARN monitor pid=${monitor_pid} 未正常退出，发送 KILL"
                kill -9 "$monitor_pid" 2>/dev/null || true
            fi
        fi
    fi

    stop_ssh_tunnel
    rm -f "$MONITOR_PID_FILE" "$SSH_PID_FILE"
    echo "board-review-tunnel 已停止"
    return 0
}

show_status()
{
    # show_status 的作用：
    #   输出守护进程、ssh 隧道和板端本地回写服务当前状态。
    # 主要流程：
    #   1. 检查 monitor PID。
    #   2. 检查 ssh 隧道 PID。
    #   3. 检查本地 review-result HTTP 服务。
    # 参数：
    #   无。
    # 返回值：
    #   monitor 和 ssh 都存在时返回 0；任一缺失返回 1。
    ensure_runtime_dir
    status_rc=0

    if monitor_is_alive; then
        echo "monitor: running pid=$(cat "$MONITOR_PID_FILE" 2>/dev/null)"
    else
        echo "monitor: stopped"
        status_rc=1
    fi

    if ssh_tunnel_is_alive; then
        echo "ssh_tunnel: running pid=$(cat "$SSH_PID_FILE" 2>/dev/null | tr '\n' ' ')"
        echo "remote_forward: ${REMOTE_FORWARD_SPEC}"
    else
        echo "ssh_tunnel: stopped"
        status_rc=1
    fi

    local_state="$(local_review_service_state 2>/dev/null || true)"
    echo "local_review_service: ${local_state}"
    echo "log_file: ${LOG_FILE}"
    return "$status_rc"
}

case "${1:-start}" in
    start)
        start_monitor
        ;;
    stop)
        stop_monitor
        ;;
    restart)
        stop_monitor
        sleep 1
        start_monitor
        ;;
    status)
        show_status
        ;;
    monitor)
        monitor_loop
        ;;
    *)
        echo "用法：$0 {start|stop|restart|status}" >&2
        exit 2
        ;;
esac
