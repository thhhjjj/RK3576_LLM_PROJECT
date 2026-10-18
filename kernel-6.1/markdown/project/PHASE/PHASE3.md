# 第三阶段：做到哪一步

> 依据 [project.md](../project.md) 的**网络层**。这一阶段结束时：局域网 WebSocket 能流式收 token；本机屏可切「本地 / 云端」；云端默认 **DeepSeek**（OpenAI 兼容 SSE），断网有状态不卡死。GATT、frpc、拼音输入法都还没开始。

第二阶段完成线见 [PHASE2.md](./PHASE2.md)；已做记录见 [PHASE2_DID.md](./PHASE2_DID.md)。**第二阶段不过线，不进本阶段。**（本地能聊 + 能压缩是硬门；WebSocket 出口绑在本地推理稳定上。）

日常差分仍以 **`Desktop/makediff`** / 云端 `RK3576_LLM_PROJECT` 为提交源；panel 快验见 [version_must_care.md](./version_must_care.md)。

## 完成线

上电且本地模型已按 PHASE2 可聊的前提下，同时满足下面七条，才算第三阶段完成，才允许进第四阶段（BLE）：

1. **局域网 WebSocket**：只听固定局域网地址上的**一个**端口（如 `0.0.0.0:8765` 或仅 `wlan0` IP）；浏览器能连上并**流式**收到与本机屏一致的 token。
2. **慢客户端 / 断连隔离**：写失败、对端关闭 → 只回收该连接；队列满 → 丢该客户端旧 token 或踢掉；**推理线程不停**；本机屏继续显示本轮。
3. **云端 DeepSeek（默认）**：`libcurl` 读 **OpenAI 兼容**流式接口（SSE）；超时 / HTTP 错误 / 中途断流 → UI 有明确状态，本轮结束；本机屏与已连 WebSocket 收到「本轮结束」，进程不崩、不卡死。API Key 用你已有的 DeepSeek Key（仅内存）。
4. **API Key**：只在进程内存；界面输入；进程退出即消失。**不写 eMMC、不写配置文件、不进日志。**
5. **本地 / 云端切换**：生成中途切换时，先 **abort / 停掉正在生成的那一路**，再改模式；对话框**只有一路文本**写入。
6. **三路扇出骨架**：token 源（本地 RKLLM **或** 云端 SSE）→ **同一套有界队列投递** → LVGL 对话框 + WebSocket 发送队列（本阶段 BLE 队列可留空接口，不实现 GATT）。
7. **不做的边界成立**：不解析「下发的临时 socket 地址」；板子**不主动去连**手机给的远端；不注册 GATT；不装/不起 frpc；不做拼音输入法。

## 四块各自停在哪

| 块 | 做到这个程度 | 这一阶段停在这里 |
|---|---|---|
| WebSocket 服务 | libwebsockets 单端口；每连接一发送队列；与推理同进程或明确 IPC | 不做鉴权服务器、不做公网暴露、不做多端口 |
| 云端 SSE | libcurl + CA；流式拼进同一 token 管线；错误态可恢复下一轮 | 不做「联网拉取 API Key」；不做多云厂商抽象 |
| UI 模式开关 | 「本地 / 云端」真正改后端；云端缺 Key 时提示不可点跑 | 不在本阶段做富输入法；提问仍可用预置「试问」+（可选）简单输入框 / adb 注入 |
| 连接与 WiFi | 依赖 PHASE1 Seekwave；可选补断线重连（PHASE1 曾留给本阶段） | 不做 BLE；不做 frpc 隧道 |

## 从第二阶段继承的硬前提

- 开机：**先 load `.rkllm` 再 LVGL**；`librkllmrt` 1.3.1 + `libgomp`；试跑 **Qwen3.5-2B W4A16_G128 @1024** 文本可聊、可压缩（见 PHASE2_DID）。
- 推理回调**只入队**；禁止在回调里画 LVGL、写 socket、做压缩。本阶段 WebSocket 发送必须在**独立消费路径**（与 LVGL drain 同级的队列消费者），不得在 `RKLLM` 回调里 `lws_write`。
- rootfs 已留门：`libwebsockets`、`libcurl`+OpenSSL+CA、`bluez5`（本阶段仍可不调用 BlueZ）。
- 超温 netlink：仍可只提示；本阶段不因 WebSocket 自动关 NPU。

## 架构口径（写死）

```text
                    ┌─ LVGL 对话框（本机屏）
 token 源 ──有界队列─┼─ WebSocket 每连接发送队列
 (RKLLM 或 SSE)      └─ （预留）BLE Notify 队列 ← 第四阶段再接
```

- **本地模式**：token 只来自 `rkllm_session` / `tokq`（可把现有 `tokq` 升为「扇出总线」，或并行多订阅者）。
- **云端模式**：token 只来自 SSE 解析线程；**不要**同时跑 RKLLM 生成往对话框写。
- 某一路断开：只丢该路队列；其它路与推理继续。

## WebSocket

- 库：已在裁剪 Buildroot 的 **libwebsockets**。
- 绑定：局域网；端口与路径写进配置或写死，记入 [PHASE3_DID.md](./PHASE3_DID.md)。
- 协议：最小即可——例如文本帧收「用户问题」、文本帧推 token / 结束 / 错误 JSON 或纯文本流；**须在 DID 写清帧格式**，避免口头约定。
- 安全：本阶段默认**无 TLS**（局域网）；不把 API Key 经 WebSocket 回传日志。
- 压测：浏览器拔掉后本机屏本轮仍能显示完（对应 project 验收第 5 条的板端侧）。

## 云端 DeepSeek（SSE，本阶段默认）

- 库：**libcurl** + CA 证书（PHASE1 已留）。
- 默认厂商：**DeepSeek**（你已有 API Key）。基址与模型名写死或配置后记入 DID（常见：`https://api.deepseek.com` + `deepseek-chat` / `deepseek-reasoner`，以官方文档为准；本阶段优先 **chat、非长思维链**，免重蹈本地 thinking 占 token）。
- 协议：**OpenAI 兼容** `chat/completions` + `stream=true`，`libcurl` 读 SSE，拼 `delta.content` 进同一扇出队列。
- 失败：UI 显示「云端超时 / HTTP xxx / 断流」；向 WebSocket 广播结束事件；允许用户改回本地再问。
- Key：屏上输入或开发期环境变量注入到进程内存（**不落盘、不进日志**）；量产仍以界面输入为准。
- 后置：GLM 等其它云端可再挂同一套「兼容 SSE」适配，不挡本阶段出口。

## UI 在本阶段要接通的部分

| 控件 / 标签 | PHASE2 | PHASE3 |
|---|---|---|
| 本地 / 云端开关 | 只改字或提示「第三阶段」 | **真正切换后端**；生成中切换先停旧路 |
| 对话框 | 本地流式 | 本地或云端二选一写入 |
| 试问 / 输入 | 预置试问 | 保留试问；可选加一行输入或 adb 注入云端问题 |
| WebSocket 状态 | 无 | 可选：连接数 / 监听中（不要做成仪表盘堆砌） |
| 压缩 / 指标 | 已通 | 保持；云端轮次不走 RKLLM 压缩水位（或明确「仅本地会话」） |

拼音输入法仍不做。正式产品输入法不挡本阶段出口。

## 不过线就不进第四阶段

对应 [project.md](../project.md) 验收第 4–5 条。下面任意一条成立，第三阶段不算完成：

- WebSocket 写失败会卡住或拖死推理线程 / 本机屏
- 云端断网导致进程退出、黑屏、或对话框永转圈无结束态
- API Key 落盘或进日志
- 本地与云端同时往对话框灌两路字
- 为「演示完整产品」提前做了 GATT / frpc，并把阶段出口绑在它们上面
- 第二阶段本地压缩 / 冷启动仍不稳定却强行开监听端口

## 细分步骤

按这个顺序做。每一步过了再做下一步。版本、端口、帧格式、`MemAvailable` 记进 [PHASE3_DID.md](./PHASE3_DID.md)。

### 1. 扇出总线（先于听端口）

1. 把现有 `tokq`（或等价）收成「多订阅者」：至少 **UI + WS** 两路；约定结束 / 错误事件。
2. 确认 RKLLM 回调路径仍然**只 push、不写 socket**。
3. 本地「试问」走新总线后，屏上行为与 PHASE2 一致（回归）。

### 2. WebSocket 听起来

4. 进程内起 lws 上下文；绑定地址与端口写死并记 DID。
5. 连接建立 / 关闭时创建销毁每连接发送队列。
6. 浏览器最小页（可放 `makediff` 静态文件用电脑打开）连上，本地试问时浏览器看到流式文本。
7. 拔掉浏览器：本机屏本轮仍完成；推理无卡死。

### 3. 上行：WS 提问 → 本地推理

8. WS 收到用户问题 → 投递与「试问」同一 `session_ask`（或等价）；忙时回错误帧。
9. 多客户端同时问：定义策略（串行排队 **或** 忙则拒绝）；禁止双 `rkllm_run` 无序写屏。

### 4. 云端 DeepSeek SSE

10. 封装 `cloud_sse`（DeepSeek / OpenAI 兼容）：base URL、model、set Key、start、abort、错误码。
11. UI 切云端：无 Key → 提示；有 Key → 下一问走 DeepSeek 流式。
12. 生成中切回本地：abort SSE，再允许本地 ask。
13. 断网 / 超时：UI + WS 结束态；再问可重试。

### 5. 模式与安全收口

14. 确认 Key 不落盘、不打日志（grep 构建产物与运行 log）。
15. 重启冷启动：不手推 so 的前提下，本地仍可聊；WS 监听按设计自启或随 panel 起（写明）。
16. （可选）WiFi 断线重连：补 PHASE1 留给本阶段的策略，不挡 WS/云端出口。
17. 核对完成线七条；**第四阶段才允许 GATT**。
18. 写入 [PHASE3_DID.md](./PHASE3_DID.md)：端口、帧格式、各次故障注入结果。

### 6. 明确不做（本阶段）

- BlueZ GATT / Notify  
- frpc / 公网隧道 / 公网 TLS 页  
- 拼音输入法、完整软键盘产品化  
- 「解析下发的 socket」、板子连临时远端  
- 为 WS 单独再 load 第二份模型句柄  

## 与 PHASE2 收口的衔接（开工前核对）

进入本阶段代码前，建议 PHASE2 至少：

- [ ] 冷启动不手推即可本地试问（或 DID 写明「模型仍 adb、收口后再烤」的例外）
- [ ] 压缩 + prefill 后可继续问（已有 `vtest：0.0.6` 方向）
- [ ] 完成线关键条在 [PHASE2_DID.md](./PHASE2_DID.md) 勾过；未勾的标例外原因  

PHASE2 未收口也可以**先写**本文件做规划；**开 WebSocket 监听**仍建议本地推理稳定后再做步骤 2。
