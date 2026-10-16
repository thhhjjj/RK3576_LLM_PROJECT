# RK3576 本地大模型交互终端：可行性

> 对照硬件「RK3576 4GB+32GB、5 寸 MIPI 720×1280、AP6256」、Buildroot + 本树 Linux 6.1，以及五阶段开发顺序做的落地判断。结论先写，后面是依据和必须改的口径。

## 结论

方案可以落地。按原定顺序做：系统层跑通，再上 RKLLM，再开网络，再开 BLE，frpc 放在最后并且默认不启动。阶段拆分见 [PHASE/PHASE1.md](PHASE/PHASE1.md) 及同目录后续阶段文档。

### 模型目标（4GB + LVGL 常驻）

| 角色 | 模型 | 量化 / 上下文 | 说明 |
|---|---|---|---|
| **正式默认** | `Qwen3-1.7B-Instruct` | `W4A16`，`max_context=1024` | 官方 Base 对标约等于 `Qwen2.5-3B-Base`；权重约 0.85GB，余量够和 LVGL 共存；支持 thinking 开关 |
| **尝鲜升级** | `Qwen3.5-2B` | `W4A16_g128`（优先），`max_context=1024` | 真·2B，多项基准高于 1.7B；原生多模态；需 **RKLLM ≥ 1.3.0**（用户态 toolkit + 板端 runtime） |
| **对照 / 压测** | `Qwen2.5-3B-Instruct` | `W4A16`，`max_context=512~1024` | 能力上限参考；内存临界，**不与 LVGL 长期并行** 作为量产默认 |
| **通路冒烟** | `Qwen2.5-0.5B-Instruct` 或 `Qwen3-0.6B` | `W4A16`，`1024` | 只验证加载 / 流式 / 压缩，不当正式能力目标 |
| **不上 4GB 默认** | `Qwen3-4B` / `Qwen3.5-4B`、7B+ | — | 与 UI 共存风险高；更大模型留给后续 3588 8G/16G |

口径纠正：Qwen3 稠密系列**没有 2B**，口语「Qwen3 2B」应对 `Qwen3-1.7B`。`Qwen3.5` 才有正式的 `2B`。

复杂长对话仍可走云端 GLM-4.7。本地负责离线、低时延、私有知识；云端补能力上限。这是产品边界，不是实现风险。

三条通路（本地屏、WebSocket、BLE Notify）同时收 token，并且互不阻塞，做得到。前提是推理回调里只入队，绘制、发包、Notify 各自在自己的队列里失败、丢弃、回收。

需要改口径的有四件事，正文按改后的口径执行：

1. 浏览器 Web Bluetooth 只在 HTTPS 或 localhost 里可用。局域网 `http://192.168.x.x` 和 frp 的公网 HTTP 都不能打开 WebBLE。
2. 对 1.7B，1024 上下文 KV 压力远小于权重与运行时；2048 可作可选压测，默认仍锁 1024 直到实测通过。
3. 屏幕就是本树现成的 5 寸 MIPI 720×1280（`F050008M01`）。UI 按这个分辨率做。
4. NPU 温度已经在内核 thermal 里。自研 `llm_ctrl.ko` 读现成 thermal zone，再在临界关机之前发告警。

---

## 本树已经有的部分

| 事项 | 本树里的现状 | 对方案的含义 |
|---|---|---|
| NPU | [`rk3576.dtsi`](../arch/arm64/boot/dts/rockchip/rk3576.dtsi) 里 `npu@27700000`，`compatible = "rockchip,rk3576-rknpu"`，双核 `npu0`/`npu1`。[`rockchip_linux_defconfig`](../arch/arm64/configs/rockchip_linux_defconfig) 打开 `CONFIG_ROCKCHIP_RKNPU=y`。SoC dtsi 里 `status = "disabled"`，板级 DTS 要打开 | RKLLM 的内核前提在。裁剪时这颗驱动不能关 |
| NPU 温度 | `npu-thermal`，传感器 `tsadc` 通道 4，critical 115°C | 温度节点用 `/sys/class/thermal/thermal_zone*` 里类型为 `npu-thermal` 的 `temp`。不要再写一套 TSADC 驱动 |
| NPU 占用 | [`rknpu_debugger.c`](../drivers/rknpu/rknpu_debugger.c) 提供 `load`。debugfs 默认开，路径 `/sys/kernel/debug/rknpu/load`。procfs 选项 `ROCKCHIP_RKNPU_PROC_FS` 默认关 | UI 要读负载的话，打开 procfs，用 `/proc/rknpu/load`。裁掉 debugfs 之后 debug 路径会消失 |
| NPU 内存 | `ROCKCHIP_RKNPU` 依赖 DRM 或 Rockchip CMA heap，默认 `ROCKCHIP_RKNPU_DRM_GEM` | 关 DRM、砍光 CMA，NPU 会起不来。单帧 buffer 省的是几 MB 显存，不能靠它给模型腾内存 |
| MIPI DSI | `dsi@27d80000`，`compatible = "rockchip,rk3576-mipi-dsi2"`。参考接法是 `simple-panel-dsi` + `panel-init-sequence` | 5 寸屏走这条，不新写 MIPI 控制器驱动 |
| 5 寸屏 | [`rk3576-kickpi-lcd-mipi-5-720-1280-F050008M01.dtsi`](../arch/arm64/boot/dts/rockchip/rk3576-kickpi-lcd-mipi-5-720-1280-F050008M01.dtsi)：720×1280、4 lane、RGB888、像素时钟 84MHz，DSI 接到 VP1。[`rk3576-kickpi-k7c.dtsi`](../arch/arm64/boot/dts/rockchip/rk3576-kickpi-k7c.dtsi) 第 19 行的 include 目前被注释 | 这就是板上的屏。打开该 include，UI 按 720×1280 做。时序和初始化序列不用再向屏厂要 |
| AP6256 WiFi | KickPi K7C：SDIO `mmc@2a320000`，`mmc-pwrseq-simple` 管 `WL_REG_ON`，bcmdhd，固件 `fw_bcm43456c5_ag.bin` + `nvram_ap6256.txt`。细节在 [wifi_device.md](wifi_device.md) | 同模组、同接法可以直接沿用。GPIO、UART 不同就要改板级 DTS，驱动不用换 |
| AP6256 蓝牙 | 数据走 UART4，`rfkill-bt` 管 `BT_REG_ON`。用户态 `brcm_patchram_plus1` 下载 `BCM4345C5.hcd` 得到 `hci0`。[`rk3576.config`](../arch/arm64/configs/rk3576.config) 里 `CONFIG_BT_HCIUART` 是关闭的 | 沿用 SDK 的 `wifibt-init.sh`。不要为了蓝牙去打开内核 `HCIUART`，那会和现有 attach 方式打架 |
| 显示框架 | `CONFIG_DRM=y`、`CONFIG_DRM_ROCKCHIP=y`、`CONFIG_DRM_PANEL_SIMPLE=y` | LVGL 走 DRM/KMS dumb buffer 即可 |

RKLLM Toolkit、`librkllm.so`、LVGL、libwebsockets、libcurl、BlueZ、frpc 都不在本内核树里，由 Buildroot 和 SDK 的 `external/` 提供。内核树能证明的是 NPU、显示、WiFi、蓝牙的底层条件。

---

## 模型与内存

### 选型依据（摘要）

- 4GB 板真正留给 LLM 的可用内存大约 **2.2~2.8GB**（裁完 rootfs、LVGL 常驻之后看 `MemAvailable` 基线）。
- `Qwen3-1.7B` W4 权重约 **0.85GB**，适合与 UI 同时驻留；官方称 Base 整体对标 `Qwen2.5-3B-Base`，并带 thinking。
- `Qwen2.5-3B` W4 权重约 **1.78GB**，能力对照有价值，但与 LVGL 长期共存偏紧。
- `Qwen3.5-2B` 能力更好看，且社区已有 **RK3576 `w4a16_g128`** 预转换；模型为原生 VLM，现成包多为 `.rkllm` + `.rknn` 成对。纯文本可先只跑语言侧，视觉与 UI 共存要单独测内存。
- RKLLM **1.2.x 不支持** Qwen3.5（Gated DeltaNet）；须 **toolkit + `librkllmrt` ≥ 1.3.0**。NPU 硬件不用换；本板 6.1 厂商 SDK 升用户态即可，不必追主线 kernel 7。

### 导出参数（正式默认：Qwen3-1.7B）

- 模型：`Qwen/Qwen3-1.7B-Instruct`
- 量化：`W4A16`（建议 `group_size=128` 若 toolkit 提供）
- `target_platform=rk3576`，`num_npu_core=2`。RK3588 的 `.rkllm` 不能直接跑
- `max_context_len=1024` 写进模型文件；运行时只能 ≤ 该值
- 可选另导一份 `2048` 仅作压力测试；失败则正式锁定 1024
- eMMC 32GB：同时保留 1.7B 正式包 + 可选 0.5B/0.6B 冒烟包 + 可选 3.5-2B 尝鲜包；不要堆未量化原版权重
- ChatML（Qwen3 系，thinking 由运行时/模板开关控制，日常默认关）：

```text
<|im_start|>system
你是嵌入式 Linux 助手，擅长 C、设备树、RK 驱动；回答简洁。<|im_end|>
<|im_start|>user
{query}<|im_end|>
<|im_start|>assistant
```

### 导出参数（尝鲜：Qwen3.5-2B）

- 上游：`Qwen/Qwen3.5-2B`（Instruct / 官方发布的端侧小模型）
- 工具链：**RKLLM ≥ 1.3.0**（与板端 runtime 同大版本）
- 优先：`W4A16_g128`、`target_platform=rk3576`；可参考社区包 `HanzoHuang/Qwen3.5-2B-RKLLM` 的 `RK3576/..._w4a16_g128.rkllm`
- 若走官方/社区 VLM 路径：语言 `.rkllm` 与视觉 `.rknn` **同 SoC 成对**，不可混 3576/3588
- 第二阶段先文本通路验收，再决定是否挂视觉；挂视觉后重新记 `MemAvailable`

### 内存粗账（量级，以板上为准）

| 项 | 大约 |
|---|---|
| Qwen3-1.7B W4 权重 | ~0.85GB |
| Qwen3.5-2B W4（语言侧） | 高于 1.7B，仍明显低于 3B；VLM 全开更紧 |
| Qwen2.5-3B W4 权重 | ~1.78GB |
| 系统 + LVGL + 驱动 + RKLLM 工作区 | 吃掉其余；以 PHASE1 裁剪后的 `MemAvailable` 为基线 |

真正占内存的是权重、RKLLM 工作区、内核和常驻服务。加载完成后、上下文跑满后再各读一次 `MemAvailable`，决定默认模型与是否允许 2048。

压缩或切换模型时，先销毁当前 RKLLM 句柄，再创建新句柄。两个句柄叠在一起会把权重量级占两份，压力测试会在峰值上失败。

### 启动顺序（防碎片，硬口径）

NPU / RKLLM 需要较大块**连续物理内存**。`MemAvailable` 够用不等于能分配成功；LVGL、字库、DRM 缓冲等先起来会加剧碎片。

**正式进程启动顺序写死：**

1. 初始化 DRM / 最小显示依赖（若加载模型必须依赖，仅保留必要部分）
2. **先加载 RKLLM 模型并建好会话句柄**
3. **再启动 LVGL**（控件、字库、指标定时器、对话框缓冲）
4. 其后才挂 WebSocket / BLE 等可推迟的服务

禁止：先拉满 LVGL UI，再在用户第一次提问时才 `load` 大模型。换模型时同样先销毁旧句柄并重新加载，再恢复 UI 侧会话状态。

### 1024 与自动压缩

1024 含 system、ChatML 标记、历史和本轮回答。多轮中文仍易顶满，自动压缩是正式功能。

压缩水位用运行时回报的 token 数，不用 `strlen/4`。建议历史到 max_context 的 70% 就压。system 始终保留。压缩后销毁并重建会话，再整段 prefill。重建会有一次停顿，1024 上可接受。

调用走原始 prompt 接口，避免运行时再套一层 chat template 导致标记重复。

### 2048 / 速度

2048 压测看：加载后与跑满后的 `MemAvailable`、多轮压缩重建是否回得去、有无句柄/文本泄漏。OOM、NPU 分配失败或可用内存单调下降 → 正式版锁 1024。

单帧显示 buffer ≈ 3.5MB，不参与该判断。

RK3576 NPU 公开峰值 6 TOPS（INT8）。UI 上 token/s 用回调计数 ÷ 墙钟，板上测。计划里不写死「8 tok/s」「10~13 tok/s」之类承诺值；1.7B / 2B 以可交互为准。

---

## 第一阶段：系统层

可以做。屏的时序已经在树里，这一阶段是把它编进板级 DTS。

### DTS 和固件

1. 打开 [rk3576-kickpi-k7c.dtsi](../arch/arm64/boot/dts/rockchip/rk3576-kickpi-k7c.dtsi) 里对 [`rk3576-kickpi-lcd-mipi-5-720-1280-F050008M01.dtsi`](../arch/arm64/boot/dts/rockchip/rk3576-kickpi-lcd-mipi-5-720-1280-F050008M01.dtsi) 的 include（当前第 19 行被注释）。该文件已给出 720×1280、4 lane、RGB888、84MHz、`simple-panel-dsi` 初始化序列，DSI 走 VP1。LVGL 画布按 720×1280 竖屏做。
2. 面板节点里的复位（`gpio0 RK_PA2`）、背光（`pwm1_6ch_1`）和电源（`gpio0 RK_PC6`）以这块板的原理图核对。脚不一致就只改这三处，不改时序。
3. 计划里的对话框、压缩按钮、模式开关需要输入设备。5 寸 MIPI 通常还有一颗 I2C 触摸（常见 GT9xx / CST / FT）。原理图上有触摸就在这一阶段一并进 DTS。没有触摸时，本地屏只做显示和少量 GPIO 键，文字从手机进来。LVGL 自带拼音输入法是单独的大项，不放进前两阶段。
4. AP6256 若与 K7C 同脚（SDIO、`WL_REG_ON`、`HOST_WAKE`、UART4、`BT_REG_ON`），直接用 [rk3576-kickpi-k7c-wifi.dtsi](../arch/arm64/boot/dts/rockchip/rk3576-kickpi-k7c-wifi.dtsi)。不同就只改 GPIO 和 pinctrl。蓝牙串口名来自 `wireless-bluetooth` 下 `uart*` 子节点，改名会导致脚本找错 `/dev/ttyS*`。
5. 固件放 rootfs：`fw_bcm43456c5_ag.bin`、`nvram_ap6256.txt`、`BCM4345C5.hcd`，由 SDK `external/rkwifibt` 安装。内核源码树里没有这些 bin。

阶段出口：MIPI 有画面，`wlan0` 能连上 AP 并拿到地址，`hci0` 出现。连上 WiFi 看 `wpa_cli status`，不要看接口的 `LOWER_UP`（bcmdhd 的载波位不能表示已经关联）。

### Buildroot 裁剪

值得做，而且应该做。4GB 上它是运行余量，不是 2048 的开关。

内核保留：`ROCKCHIP_RKNPU`、DRM/Rockchip、MIPI DSI、`simple-panel-dsi`、`dw_mmc`、bcmdhd 依赖的 SDIO 和 rfkill、thermal、devfreq。建议打开 `CONFIG_ROCKCHIP_RKNPU_PROC_FS`，让负载读 `/proc/rknpu/load`。

rootfs 保留：`wpa_supplicant`、`bluez5`（后面 GATT 要用）、`libdrm`、`librkllm`、`libwebsockets`、`libcurl`（OpenSSL）、CA 证书、`brcm_patchram_plus1` 和上述固件。中文字体用子集，整包思源黑体没有必要。

CMA / DMA heap 维持 SDK 默认，等 NPU 加载模型失败再调，不预先砍小。

显示可以用单块 dumb buffer（720×1280×4 ≈ 3.5MB）。会有撕裂。双缓冲大约再多一块同样大小，相对 4GB 可以忽略。单缓冲作为省内存策略可以留，不要把它写成 2048 可行的条件。

### `llm_ctrl.ko`

作为开机加载的 platform 字符设备，可行，职责收窄：

- 找到 `npu-thermal` 对应的 thermal zone，读毫摄氏度
- 在低于内核 critical（115°C）的阈值，例如 85°C 或 95°C，用 genetlink 通知用户态
- sysfs 同时给出当前温度，LVGL 轮询这条即可；netlink 只做超温事件
- 不自己映射 TSADC，不接管已经属于 thermal 子系统的传感器

内核到 115°C 会走 critical 关机。用户态告警是提前量。

### LVGL

DRM 后端初始化、指标面板，在第一阶段完成。对话控件先摆出来，回调先空着。

| 指标 | 来源 |
|---|---|
| 内存 | `/proc/meminfo` 的 `MemTotal`、`MemAvailable` |
| CPU | `/proc/stat` 两次采样做差 |
| NPU 负载 | `/proc/rknpu/load`（打开 procfs 之后） |
| NPU 温度 | `llm_ctrl` 的 sysfs，或直接读 thermal zone |
| GPU 负载 | `/sys/class/devfreq/` 下 GPU 节点的 `load`，节点名以板上为准 |
| 上下行 | `/proc/net/dev` 对 `wlan0` 两次采样做差 |

LVGL 不是线程安全的。采集放定时器，后面推理线程只许 `lv_async_call` 或投递到 LVGL 线程。

---

## 第二阶段：本地推理

这是主路径，和本树的 NPU 驱动匹配。风险在 API 用法、会话生命周期和 **RKLLM 版本与模型匹配**，不在「NPU 硬件够不够」。

1. PC 与板端 RKLLM **同大版本**。跑 Qwen3-1.7B 可用现有 1.2.x+（以实际支持列表为准）；跑 **Qwen3.5-2B 必须 ≥ 1.3.0**。版本不一致是加载失败的常见原因。升 toolkit / `librkllmrt.so` 即可，一般不必动内核 rknpu。
2. 落地顺序：先 **0.5B/0.6B 冒烟** → **Qwen3-1.7B 正式**（与 LVGL 同驻）→ 可选 **Qwen3.5-2B** 文本压测 → 再决定是否挂视觉 / 是否保留 3B 对照包。
3. **进程内启动顺序：先加载模型，再开 LVGL**（见上文「启动顺序」）。这是防 CMA/NPU 连续内存碎片的硬要求，不是优化项。
4. 封装：加载、建会话、流式推理。回调累计 token 与时间得 token/s。thinking 默认关；UI 可提供高级开关。
5. 消息列表在进程内存；prompt 为按 ChatML 拼好的普通字符串。
6. 自动/手动压缩同一函数：保留 system，删最早轮次，销毁重建会话再 prefill。
7. 内存保护：周期看 `MemAvailable`。低于阈值（建议先 200MB，按曲线改）时 UI 告警并压缩；压缩函数重入保护。

回调里禁止：画 LVGL、写 socket、写 GATT、做压缩。回调只入有界队列。

模式切换和停止时先 abort，等本轮回调结束再销毁句柄。切换模型禁止双句柄并存。

阶段出口：默认 **Qwen3-1.7B @1024** 多轮对话，自动/手动压缩后可继续，`MemAvailable` 压缩后回升，LVGL 指标面板仍刷新，进程不退出。Qwen3.5-2B 作为加分项，不挡阶段出口。

---

## 第三阶段：网络

WiFi 管理沿用 `wpa_supplicant`。独立监控里看 `wpa_state`，断开再拉起，避免在已连接时反复重配。UI 显示地址、信号（`wpa_cli signal_poll` 或 nl80211）、连接状态。

WebSocket 用 libwebsockets，只听局域网地址上的一个端口。每个客户端一块发送队列。写失败、对端关闭，只回收这个连接。慢客户端队列满了就丢掉该客户端的旧 token 或关掉它，推理线程继续跑。

云端 GLM-4.7 用 libcurl 读 SSE。超时、HTTP 错误、中途断流，在 UI 上显示状态，并结束这一轮，本地屏和已经连上的 WebSocket 收到「本轮结束」而不是卡死。

API Key 只放进程内存，界面输入，进程退出即消失。不写 eMMC、不写配置文件、不进日志。原清单里的「联网拉取 API Key」和「界面输入」二选一，本方案用界面输入。联网拉取要多一个密钥来源和常驻客户端，不进这五阶段。

UI 切换「本地 Qwen3-1.7B（或已启用的板端模型）/ 云端 GLM-4.7」时，先停掉正在生成的那一路，再改模式。两条流不能同时往对话框里写。

「解析下发的 socket」没有协议，不做。远程用户连板子上的 WebSocket，板子不去连一个临时下发的地址。

---

## 第四阶段：BLE

AP6256 的 `hci0` 起来之后，用 BlueZ GATT 做外设：一个可写特征收问题，一个 Notify 特征推 token。Notify 按协商后的 MTU 分片。

BLE 队列独立。手机走远、Notify 写不进去，丢的是蓝牙这一路的 token，LVGL 和 WebSocket 继续。

Web Bluetooth 的限制改成下面这样：

| 页面从哪打开 | 浏览器能否调用 Web Bluetooth |
|---|---|
| `https://`（本机证书或 frp 上的 TLS） | 可以 |
| `http://localhost` | 可以 |
| `http://192.168.x.x` | 不行 |
| frp 的公网 `http://` | 不行 |

蓝牙射频不经过 frp，也不经过板子上的 HTTP 服务。页面只是负责在手机浏览器里拿到调用许可。手机和板子不在蓝牙距离内，HTTPS 也连不上模组。人在远处时走 WebSocket；人在旁边、又希望用浏览器蓝牙时，页面用 HTTPS。

原生 App 或 nRF Connect 直接连 GATT，不经过这个安全上下文限制。调试阶段可以先用它们证明 Notify 通路，再做浏览器页面。

---

## 第五阶段：frpc

可选，优先级最低。arm64 的 frpc 放进 rootfs，UI 按钮启动和 `kill`，默认不启动，也不写进开机脚本。关掉时回收进程。

隧道验证的是：手机用蜂窝流量打开公网地址，页面和 WebSocket 能完成一轮对话。frp 的 HTTP 类型要能升级 WebSocket；拿不准时用 TCP 转发整个端口，少一层代理。

这一阶段仍然不让 WebBLE 在公网 HTTP 上工作。若公网页面也要 WebBLE，隧道改成 TLS，并且测试手机必须在板子旁边。

frpc 本身大约几十 MB 级，不是内存大户。默认关闭是为了少一个常驻进程和一条暴露面。

---

## 三条通路

```text
RKLLM 回调 / curl SSE 读线程
        │  只写入有界队列（本地、WebSocket、BLE 各一队）
        ├─ LVGL 线程：刷新对话框和 token/s
        ├─ libwebsockets 线程：按连接发送，失败只关这一连接
        └─ BlueZ 线程：Notify，失败只停 BLE
```

生成进行中，某一路断开，不调用会卡住推理的发送函数。本地模式和云端模式共用这三队，区别只是 token 从 RKLLM 来还是从 SSE 来。

---

## 原计划里要改的口径

| 原写法 | 执行口径 |
|---|---|
| 正式模型 `Qwen2.5-0.5B` | 正式默认改为 `Qwen3-1.7B-Instruct` W4A16 @1024；0.5B/0.6B 仅冒烟；可选尝鲜 `Qwen3.5-2B`（RKLLM≥1.3.0） |
| 先开 UI 再按需加载模型 | **先加载 RKLLM，再启动 LVGL**，降低 NPU 连续内存分配失败风险 |
| 局域网 HTTP 可用 WebBLE，公网 HTTP 不可用 | 浏览器 WebBLE 要 HTTPS 或 localhost。局域网 HTTP 同样不可用。远处手机走 WebSocket |
| 2048 必须深度裁剪才有希望，否则 OOM | 默认锁 1024；2048 仅压测。OOM 主因是权重/运行时/双句柄，不是「多 12MB KV」。压缩时禁止两个 RKLLM 句柄并存 |
| 屏是 720×1080 | 用本树 5 寸 MIPI 720×1280（`F050008M01`）。打开 K7C 里被注释的 include，UI 按 720×1280 做 |
| 自研驱动直接做 NPU 温度采集 | 读 `npu-thermal`，阈值告警走 netlink。critical 115°C 仍由内核处理 |
| DRM 单帧 buffer 是省内存的关键 | 只能省约一块帧缓冲。CMA 和 RKNPU 维持默认为主 |
| 联网拉取 API Key，同时又在 UI 输入且不落盘 | 只保留 UI 输入，进程内存，不落盘 |
| 裁掉调试用文件系统 | 可以关无关调试项。NPU 负载改走 `/proc/rknpu/load`，避免绑死 debugfs |
| 蓝牙依赖内核 HCI UART | 本配置关闭了 `CONFIG_BT_HCIUART`。用 `brcm_patchram_plus1` + `BCM4345C5.hcd` |

---

## 裁剪红线

关闭这些会导致对应阶段直接失败，而不是「变慢」：

- `CONFIG_ROCKCHIP_RKNPU`、NPU 的电源域和 IOMMU（板级把 `rknpu` 设为 okay）
- DRM、Rockchip VOP/DSI、面板驱动（RKNPU 默认还依赖 DRM GEM）
- SDIO、`mmc-pwrseq-simple`、rfkill、bcmdhd 模块
- 蓝牙固件和 `brcm_patchram_plus1` 用的那路 UART
- `MemAvailable`、thermal、devfreq、`/proc/net/dev` 这些统计源所在的 procfs / sysfs

---

## 验收

与原测试清单对应，判定写死，避免「能亮就算过」。

1. 上电：MIPI 点亮为 720×1280；`wlan0` 关联完成并有 IPv4；`hci0` 存在；LVGL 上内存、CPU、NPU、GPU、网速为合理数值（空载 NPU 接近 0，断网时网速为 0）。
2. 默认模型 `Qwen3-1.7B` @1024：连续多轮触发自动压缩，再手动压缩一次。压缩后还能回答，`MemAvailable` 不单调下降，无内核 OOM；LVGL 指标仍刷新。
3. 可选：同一套压缩流程对 `2048` 测试包、或对 `Qwen3.5-2B`（RKLLM≥1.3.0）做压测。失败不挡主路径，正式版保持 1.7B@1024，记录 `MemAvailable` 与 dmesg。
4. 模式切换：本地与 GLM-4.7 各完成一轮流式输出。切换发生在生成中途时，旧的一路停止，对话框只有一路文本。断网时云端路径给出错误状态，进程仍在。
5. 局域网 WebSocket：浏览器收全量流式文本。拔掉客户端，本地屏继续把这一轮显示完。
6. BLE：手机在旁边，写特征提问，Notify 能收到后续 token。关掉手机蓝牙，本地屏和 WebSocket 不受影响。浏览器路径另在 HTTPS 页面上复测。
7. frpc：默认开机没有 frpc 进程。UI 打开后，蜂窝流量能完成 WebSocket 对话。UI 关闭后进程消失，`MemAvailable` 回升。

第 1 项不过，不进模型。第 2 项不过，不开 WebSocket。第 5 项不过，不做 frpc。
