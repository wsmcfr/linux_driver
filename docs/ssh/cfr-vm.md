# cfr-vm SSH 连接说明

## 目的

把 STM32MP157 开发虚拟机的连接信息固定下来，后续会话直接用 `ssh cfr-vm` 进入虚拟机，不再重复找主机地址。

## 连接信息

| 项目 | 值 |
|---|---|
| Host alias | `cfr-vm` |
| HostName | `192.168.234.130` |
| User | `cfr` |
| Port | `22` |
| IdentityFile | `C:\Users\caofengrui\.ssh\id_ed25519_cfr_vm` |
| IdentitiesOnly | `yes` |

## 使用方法

1. 确认本机已经存在上面的私钥文件路径。
2. 确认本机 SSH 配置里有 `Host cfr-vm` 这一段。
3. 直接执行：

```bash
ssh cfr-vm
```

## 注意

- 这里只记录连接方式，不保存私钥明文。
- 如果密钥轮换，只更新本机 `IdentityFile` 指向的文件，别名保持不变。
- 如果需要排查连接是否生效，可以先执行 `ssh -G cfr-vm` 查看解析后的主机参数。
