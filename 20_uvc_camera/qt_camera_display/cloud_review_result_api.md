# 云端复核结果回写板端接口说明

## 目标

| 项目 | 内容 |
|---|---|
| 目标 | 云端判定结果和板端原始检测结果不一致时，云端详情页点击一个按钮，把云端复核结论和修改原因回写到板端同一条检测记录。 |
| 板端记录 | `/mnt/sdcard/images/upload_history.json` |
| 板端匹配键 | 优先 `record_id`，兜底 `record_no` |
| 板端接口 | `POST /api/v1/review-result` |
| 默认板端地址 | `http://<board-ip>:18080/api/v1/review-result` |

## 正式部署链路

| 环节 | 运行位置 | 责任 | 说明 |
|---|---|---|---|
| Qt 回写服务 | STM32MP157 开发板 | 监听 `127.0.0.1:18080` 或 `0.0.0.0:18080` | 由 `qt_camera_display` 内置 HTTP 服务提供，只接受 `POST /api/v1/review-result`。 |
| 反向 SSH 隧道 | STM32MP157 开发板 | 主动连接云服务器并维持 `ssh -R` | `/etc/init.d/S91board-review-tunnel` 开机启动 `/root/qt_camera_display/board-review-tunnel.sh`，建立 `127.0.0.1:18081 -> 127.0.0.1:18080`。 |
| 云端后端 | 云服务器 | 常驻运行并调用回写 URL | `yunduan-backend.service` 已设置 `Restart=always` 和开机自启；设备 `board_review_url` 应配置为 `http://127.0.0.1:18081/api/v1/review-result`。 |
| 云端检查 | 云服务器 | 周期检查隧道是否可用 | `yunduan-board-review-tunnel-check.timer` 每 60 秒执行一次 `/opt/yunduan/scripts/check_board_review_tunnel.sh`，失败时写日志。 |

注意：最终现场运行时不启动 Windows 或虚拟机。板端在有网络后主动连云端，云端不能凭空主动连回 4G/NAT 后面的板端；如果隧道断开，真正的重连动作由板端 `board-review-tunnel.sh` 完成，云端定时任务只负责检测、记录和辅助排障。

云端后端、前端、数据库和测试的详细适配步骤已经单独写入云端项目文档：

| 文档 | 说明 |
|---|---|
| `D:\yunfuwu\docs\stm32mp157-board-review-sync-adaptation.md` | 说明云端需要新增哪些数据库字段、FastAPI 接口、后端服务逻辑、Vue 按钮弹窗、设备回写配置、零件类型规则和验收测试。 |

## 板端配置

| 配置项 | 默认值 | 说明 |
|---|---|---|
| `BOARD_REVIEW_ENABLE` | `1` | 设为 `0` 可关闭回写接口。 |
| `BOARD_REVIEW_LISTEN` | `0.0.0.0` | 监听地址，现场只允许本机代理访问时可设为 `127.0.0.1`。 |
| `BOARD_REVIEW_PORT` | `18080` | 监听端口。 |
| `BOARD_REVIEW_TOKEN` | 空 | 正式部署必须设置；云端通过 `X-Board-Token` 请求头携带。 |

`BOARD_REVIEW_TOKEN` 可以写入 `/root/qt_camera_display/cos-upload.env`：

```sh
BOARD_REVIEW_TOKEN='please-change-this-token'
```

## 云端按钮交互

| 步骤 | 云端行为 |
|---|---|
| 1 | 用户在云端检测详情页确认云端最终结果，例如 `bad`。 |
| 2 | 用户点击一个按钮，例如“修正板端结果”。 |
| 3 | 云端弹出原因输入框，要求填写为什么要修改，例如“云端复核发现边缘划痕”。 |
| 4 | 用户确认后，云端后端保存自己的复核记录。 |
| 5 | 云端后端调用板端 `POST /api/v1/review-result`，把结果和原因传给板端。 |
| 6 | 板端更新本地历史 JSON，并在历史详情显示“云端修正”。 |

注意：不是五个按钮，只需要一个“修正板端结果”按钮；按钮点击后必须弹出原因输入框。

## 零件类型上传规则

| 项目 | 规则 |
|---|---|
| 云端统计维度 | `POST /api/v1/records` 的顶层 `part_id`，不是 `good/bad` 结果文本。 |
| 板端传参 | Qt 从分类模型 `class=` 提取零件类型，去掉最后的 `_good/_bad/-good/-bad` 后缀后，通过 `CLOUD_PART_CODE` 传给上传脚本。 |
| 示例 | `gasket_good` 和 `gasket_bad` 都上传为零件类型 `gasket`；`washer_good` 才是另一个零件类型 `washer`。 |
| 云端零件表 | 云端只为不同零件创建不同 `parts` 记录，例如 `gasket`、`washer`、`splitwasher`、`wave_washer`；不要为 `gasket_good` 和 `gasket_bad` 创建两条零件。 |
| 显示名约定 | 历史训练编码 `gasket` 实际代表波形垫圈，云端和板端界面都应显示为 `波形垫圈`；`washer` 显示为 `平垫圈`，`splitwasher` 显示为 `弹性垫圈`。 |
| 大类约定 | `垫圈类` 是统计和零件管理入口大类；`波形垫圈`、`平垫圈`、`弹性垫圈` 是该大类下的具体零件类型。 |
| 创建记录 | 上传脚本先调用 `GET /api/v1/parts?limit=100`，按 `part_code/name/category` 匹配零件类型；匹配到时发送 `part_id`，匹配不到时在 `POST /api/v1/records` 中发送 `part_code/part_name/part_category/auto_create_part=true`，由云端自动创建或复用零件。 |
| 排障字段 | 板端会把归一后的 `part_code` 和原始 `class_label` 放入 `device_context`，方便云端排查模型标签和零件映射。 |
| 匹配失败 | 不再让板端上传失败；只要板端传入 `CLOUD_PART_CODE` 或 `CLOUD_CLASS_LABEL`，脚本就会走自动创建零件路径。只有完全缺少零件类型时才需要现场设置 `CLOUD_PART_ID` 或补传零件类型。 |

创建检测记录时，云端已有零件的板端请求体会包含类似字段：

```json
{
  "part_id": 11,
  "device_id": 1,
  "result": "bad",
  "device_context": {
    "part_code": "gasket",
    "class_label": "gasket_bad"
  }
}
```

这里 `result=bad` 只表示本次检测结果，`part_id=11` 仍然是 `gasket` 这一种零件。

如果云端还没有对应零件，板端会改发自动创建字段：

```json
{
  "part_code": "wave_washer",
  "part_name": "波形垫圈",
  "part_category": "垫圈类",
  "auto_create_part": true,
  "device_id": 1,
  "result": "good",
  "device_context": {
    "part_code": "wave_washer",
    "class_label": "wave_washer_good"
  }
}
```

这里 `wave_washer` 是零件类型，`wave_washer_good` 是模型原始类别；云端创建的零件名称必须是 `波形垫圈`，大类必须是 `垫圈类`，不能翻译成“电平”或落回“垫片”。

## 请求格式

```http
POST /api/v1/review-result HTTP/1.1
Host: 192.168.1.250:18080
Content-Type: application/json
X-Board-Token: please-change-this-token
```

```json
{
  "record_id": "123",
  "record_no": "REC-20260519-0001",
  "cloud_result": "bad",
  "cloud_reason": "云端复核发现边缘划痕，板端原判良品需要修正。",
  "operator": "admin",
  "review_time": "2026-05-19 14:03:10",
  "source": "cloud"
}
```

| 字段 | 必填 | 说明 |
|---|---|---|
| `record_id` | 条件必填 | 云端记录 ID，优先用于匹配板端本地历史。 |
| `record_no` | 条件必填 | 云端记录编号；`record_id` 为空时兜底匹配。 |
| `cloud_result` | 是 | 只能是 `good`、`bad`、`review`。 |
| `cloud_reason` | 是 | 云端按钮弹窗填写的修改原因，板端为空会拒绝。 |
| `operator` | 否 | 云端操作人账号或姓名。 |
| `review_time` | 否 | 云端复核时间；为空时板端使用本地当前时间。 |
| `source` | 否 | 默认 `cloud`。 |

## 响应格式

成功：

```json
{
  "ok": true,
  "message": "updated",
  "updated": true,
  "record_id": "123",
  "record_no": "REC-20260519-0001",
  "board_result_text": "良品",
  "effective_result_text": "坏品",
  "cloud_review_result": "bad"
}
```

失败：

```json
{
  "ok": false,
  "error": "云端修正原因不能为空"
}
```

| 状态码 | 含义 | 云端处理建议 |
|---|---|---|
| `200` | 板端历史已更新 | 云端记录 `board_sync_status=success`，保存同步时间。 |
| `400` | 参数错误 | 提示用户补充原因或检查结果枚举。 |
| `401` | token 错误 | 提示检查设备密钥配置。 |
| `404` | 板端找不到记录 | 提示该记录可能不在这块板端或本地历史已删除。 |
| `500` | 板端内部错误 | 保留云端复核结果，允许稍后重试同步。 |

## 反向隧道部署与验证

### 板端部署

| 文件 | 作用 |
|---|---|
| `/root/qt_camera_display/board-review-tunnel.sh` | 板端守护脚本，周期检查 `ssh -R` 进程，断线后自动重连。 |
| `/etc/init.d/S91board-review-tunnel` | Buildroot SysV 开机入口，调用守护脚本 `start/stop/restart/status`。 |
| `/root/.ssh/id_ed25519_yunfuwu_tunnel` | 板端专用隧道私钥，只用于连接云端建立反向转发。 |

板端常用命令：

```sh
/etc/init.d/S91board-review-tunnel restart
/etc/init.d/S91board-review-tunnel status
tail -n 80 /tmp/board-review-tunnel.log
```

状态输出应包含：

```text
monitor: running
ssh_tunnel: running
remote_forward: 127.0.0.1:18081:127.0.0.1:18080
```

### 云端部署

| 文件 | 作用 |
|---|---|
| `/opt/yunduan/scripts/check_board_review_tunnel.sh` | 云端本机检测 `127.0.0.1:18081` 是否监听并能转发到板端。 |
| `/etc/systemd/system/yunduan-board-review-tunnel-check.service` | systemd oneshot 检查服务。 |
| `/etc/systemd/system/yunduan-board-review-tunnel-check.timer` | 每 60 秒触发一次检查服务。 |
| `/var/log/yunduan-board-review-tunnel-check.log` | 云端周期检查日志。 |

云端常用命令：

```sh
systemctl status yunduan-backend nginx yunduan-board-review-tunnel-check.timer
systemctl list-timers --all | grep yunduan-board-review
ss -ltnp | grep 127.0.0.1:18081
curl -i --max-time 5 http://127.0.0.1:18081/api/v1/review-result
tail -n 80 /var/log/yunduan-board-review-tunnel-check.log
```

`GET /api/v1/review-result` 返回 `404` 是正常现象，因为板端接口只接受 `POST`；只要 HTTP 有响应，就说明云端已经通过反向隧道访问到了板端服务。

## 隧道验证矩阵

| 测试目标 | 执行位置 | 命令 | 预期输出/现象 | 失败时排查 |
|---|---|---|---|---|
| 板端 Qt 回写服务 | 开发板 | `curl -i --max-time 5 http://127.0.0.1:18080/api/v1/review-result` | 返回 HTTP 404 JSON，说明本地服务可达。 | 查 `pidof qt_camera_display`、`/tmp/qt_camera_display.log` 和 `BOARD_REVIEW_ENABLE/PORT`。 |
| 板端隧道守护 | 开发板 | `/etc/init.d/S91board-review-tunnel status` | `monitor: running` 且 `ssh_tunnel: running`。 | 查 `/tmp/board-review-tunnel.log`、私钥权限、4G 网络和云端 `authorized_keys`。 |
| 云端端口监听 | 云服务器 | `ss -ltnp | grep 127.0.0.1:18081` | 看到 `sshd` 监听 `127.0.0.1:18081`。 | 若无监听，等待板端守护重连；若端口被旧隧道占用，先停止旧 SSH 会话。 |
| 云端转发到板端 | 云服务器 | `curl -i --max-time 5 http://127.0.0.1:18081/api/v1/review-result` | 返回板端 HTTP 404 JSON。 | 若连接拒绝，查板端守护；若超时，查 4G/SSH；若 502/000，查 Qt 回写服务。 |
| 云端周期检查 | 云服务器 | `systemctl status yunduan-board-review-tunnel-check.timer; tail -n 20 /var/log/yunduan-board-review-tunnel-check.log` | timer active，日志周期出现 `OK 反向隧道可达`。 | 查 systemd timer 是否 enabled、脚本权限和 `/opt/yunduan/scripts/` 路径。 |

## 云端建议新增字段

| 表/结构 | 字段 | 说明 |
|---|---|---|
| `devices` | `board_review_url` | 板端回写地址，例如 `http://192.168.1.250:18080/api/v1/review-result`。 |
| `devices` 或密钥表 | `board_review_token` | 板端回写 token，只允许后端读取。 |
| `records` | `review_result` | 云端最终复核结果：`good/bad/review`。 |
| `records` | `review_reason` | 用户在弹窗中填写的修改原因。 |
| `records` | `review_operator_id` | 操作人。 |
| `records` | `review_time` | 云端复核时间。 |
| `records` | `board_sync_status` | `pending/success/failed`。 |
| `records` | `board_sync_time` | 最近一次同步板端成功时间。 |
| `records` | `board_sync_error` | 最近一次同步失败原因。 |

## 云端后端伪代码

```text
POST /api/v1/records/{id}/sync-board-review
1. 校验当前用户有复核权限。
2. 读取请求体 cloud_result 和 cloud_reason。
3. cloud_reason 为空时拒绝。
4. 保存 records.review_result/review_reason/review_operator_id/review_time。
5. 查找该记录对应设备的 board_review_url 和 board_review_token。
6. 向板端 POST /api/v1/review-result。
7. 板端返回 200 时写 board_sync_status=success。
8. 板端返回非 200 或超时时写 board_sync_status=failed 和 board_sync_error。
```

## 板端测试命令

在开发板执行，先确认 Qt 程序正在运行：

```sh
pidof qt_camera_display
```

用一条真实 `record_id` 测试：

```sh
curl -fsS \
  -H 'Content-Type: application/json' \
  -H 'X-Board-Token: please-change-this-token' \
  -d '{
        "record_id":"123",
        "cloud_result":"bad",
        "cloud_reason":"云端复核发现边缘划痕，修正为坏品。",
        "operator":"admin",
        "review_time":"2026-05-19 14:03:10"
      }' \
  http://127.0.0.1:18080/api/v1/review-result
```

预期输出包含：

```json
{"ok":true,"message":"updated"
```

验证本地 JSON：

```sh
grep -n '"cloud_review_result"\|"cloud_review_text"\|"board_result_text"' /mnt/sdcard/images/upload_history.json
sync
```

如果返回 `记录不存在`，先检查云端传入的 `record_id` 是否等于板端历史中的 `record_id`。
