#!/bin/sh
#
# 作用：
#   在云服务器上定时检查板端复核回写反向隧道是否可用。
#
# 重要边界：
#   云端只能检查 127.0.0.1:18081 是否还在监听、是否能转到板端 HTTP 服务；
#   如果隧道断开，真正的重连动作由板端 board-review-tunnel.sh 守护进程完成。

# 遇到未处理错误立即退出，避免 systemd 把失败检查误判为成功。
set -eu

# TUNNEL_HOST 是云端本机回环地址；authorized_keys 只允许监听这个地址。
TUNNEL_HOST="${TUNNEL_HOST:-127.0.0.1}"

# TUNNEL_PORT 是云端给后端访问的本机端口。
TUNNEL_PORT="${TUNNEL_PORT:-18081}"

# CHECK_URL 是云端通过反向隧道访问板端回写服务的探测地址。
CHECK_URL="${CHECK_URL:-http://${TUNNEL_HOST}:${TUNNEL_PORT}/api/v1/review-result}"

# LOG_FILE 保存云端周期检查结果，便于排查“按钮回写板端失败”的时间点。
LOG_FILE="${LOG_FILE:-/var/log/yunduan-board-review-tunnel-check.log}"

log_msg()
{
    # log_msg 的作用：
    #   写入带时间戳的云端检查日志，同时输出到 stdout 供 systemd journal 收集。
    # 主要流程：
    #   1. 生成当前时间。
    #   2. 拼接调用者传入的日志文本。
    #   3. 写入 LOG_FILE，并同步输出。
    # 参数：
    #   $* 是日志正文。
    # 返回值：
    #   写日志失败不阻塞 stdout 输出。
    now="$(date '+%Y-%m-%d %H:%M:%S' 2>/dev/null || date)"
    line="${now} $*"
    printf '%s\n' "$line"
    printf '%s\n' "$line" >> "$LOG_FILE" 2>/dev/null || true
}

check_port_listening()
{
    # check_port_listening 的作用：
    #   判断云端本机 127.0.0.1:18081 是否存在监听。
    # 主要流程：
    #   1. 优先使用 ss -ltn 检查监听端口。
    #   2. 精简系统没有 ss 时跳过该层，只依赖 curl 探测。
    # 参数：
    #   无。
    # 返回值：
    #   端口监听或无法使用 ss 时返回 0；明确未监听返回 1。
    if ! command -v ss >/dev/null 2>&1; then
        log_msg "WARN ss 命令不存在，跳过端口监听层检查"
        return 0
    fi

    if ss -ltn | grep -q "${TUNNEL_HOST}:${TUNNEL_PORT}"; then
        return 0
    fi

    log_msg "ERROR 云端未监听 ${TUNNEL_HOST}:${TUNNEL_PORT}；等待板端守护进程重新建立 ssh -R"
    return 1
}

check_http_forward()
{
    # check_http_forward 的作用：
    #   验证云端通过 18081 访问到的是板端 HTTP 服务，而不是空端口。
    # 主要流程：
    #   1. 用 curl GET /api/v1/review-result。
    #   2. 板端该接口只接受 POST，因此 GET 返回 404 属于“服务可达”的正常结果。
    #   3. 连接失败、超时或 000 才判为隧道不可用。
    # 参数：
    #   无。
    # 返回值：
    #   HTTP 2xx/3xx/4xx 返回 0；其它返回 1。
    if ! command -v curl >/dev/null 2>&1; then
        log_msg "ERROR curl 命令不存在，无法验证反向隧道 HTTP 转发"
        return 1
    fi

    http_code="$(curl -sS -o /tmp/yunduan-board-review-tunnel-check.body -w '%{http_code}' --max-time 5 "$CHECK_URL" 2>/tmp/yunduan-board-review-tunnel-check.err || echo 000)"
    case "$http_code" in
        2*|3*|4*)
            log_msg "OK 反向隧道可达 url=${CHECK_URL} http_code=${http_code}"
            return 0
            ;;
        *)
            error_text="$(head -c 200 /tmp/yunduan-board-review-tunnel-check.err 2>/dev/null || true)"
            log_msg "ERROR 反向隧道 HTTP 检查失败 url=${CHECK_URL} http_code=${http_code} error=${error_text}"
            return 1
            ;;
    esac
}

# 先检查监听，再检查 HTTP 转发；两层都通过才认为云端侧可用。
check_port_listening
check_http_forward
