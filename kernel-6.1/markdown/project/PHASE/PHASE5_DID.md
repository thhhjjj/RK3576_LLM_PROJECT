# PHASE5 已做记录

> 对照 [PHASE5.md](./PHASE5.md)。PHASE4 BLE **已取消**。

## 进度

| 项 | 状态 |
|---|---|
| PHASE4 跳过 | **已记** |
| frpc 起停库 `frpc_ctl.c` | **代码已加**（0.9.31） |
| 设置页开关 | **代码已加** |
| `/etc/frpc.toml` 模板 | **example 已加**；板上待填 server |
| arm64 `frpc` 二进制进板 | **待推** `/usr/bin/frpc` |
| 蜂窝一轮对话 | **待**（需 frps） |
| 开机默认不起 | **保持**（无 init 脚本） |

## 配置口径

- 类型：**TCP**，`localPort=8765` → `remotePort`（建议 18765）
- 二进制：`/usr/bin/frpc`
- 配置：`/etc/frpc.toml`（`chmod 600`，token 勿进 git）
- 模板：`llm_panel/rootfs_wifi/etc/frpc.toml.example`

## 你这边还差

1. 一台跑 **frps** 的机器（VPS）：`bindPort=7000`，`auth.token=…`
2. 把板子 `/etc/frpc.toml` 里 `CHANGE_ME` 换成真实 `serverAddr` / `token` / `remotePort`
3. 重编推 **0.9.31** → 设置页开 frpc → 蜂窝用 `ws://公网IP:remotePort/` 连试连页

## 明确不做

- 开机自启 frpc  
- PHASE4 GATT  
- 公网 HTTP WebBLE  
