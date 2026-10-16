# RK3576 本地大模型交互终端：可行性

> 对照硬件「RK3576 4GB+32GB、5 寸 MIPI 720×1280、AP6256」、Buildroot + 本树 Linux 6.1，以及五阶段开发顺序做的落地判断。结论先写，后面是依据和必须改的口径。

## 结论

方案可以落地。按原定顺序做：系统层跑通，再上 RKLLM，再开网络，再开 BLE，frpc 放在最后并且默认不启动。

正式模型用 `Qwen2.5-0.5B-Instruct`、`W4A16`、导出时写死 `max_context=1024`。另导出一份 `2048` 做压力测试。4GB 内存上，2048 的稳态占用增加很小，默认仍等压力测试通过再切；测试失败就锁定 1024。

三条通路（本地屏、WebSocket、BLE Notify）同时收 token，并且互不阻塞，做得到。前提是推理回调里只入队，绘制、发包、Notify 各自在自己的队列里失败、丢弃、回收。

需要改口径的有四件事，正文按改后的口径执行：

1. 浏览器 Web Bluetooth 只在 HTTPS 或 localhost 里可用。局域网 `http://192.168.x.x` 和 frp 的公网 HTTP 都不能打开 WebBLE。
2. 2048 相对 1024，KV 大约只多 12MB。把它当成「整机深度裁剪之后才有希望」会高估内存压力。
3. 屏幕就是本树现成的 5 寸 MIPI 720×1280（`F050008M01`）。UI 按这个分辨率做。
4. NPU 温度已经在内核 thermal 里。自研 `llm_ctrl.ko` 读现成 thermal zone，再在临界关机之前发告警。

本地 0.5B 适合短问答和离线演示。复杂对话走云端 GLM-4.7。这是能力边界，不是实现风险。

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

### 导出参数

- 模型：`Qwen2.5-0.5B-Instruct`
- 量化：`W4A16`（权重 INT4，激活 FP16）
- `target_platform` 必须是 RK3576。RK3588 的 `.rkllm` 不能拿来直接跑
- `max_context` 在 PC 端转换时写进模型文件。运行时只能小于或等于这个值，不能在板上改大
- 正式文件和测试文件各一份，都放 eMMC。32GB 足够，一份 W4A16 的 0.5B 模型是几百 MB 这一档，导出后用文件大小核对
- 结构（官方配置）：24 层，hidden 896，14 个 query head，2 个 KV head，head_dim 64，词表 151936

### KV 账

激活按 FP16 计，每个 token 的 K+V：

```text
2 × 24 层 × 2 KV head × 64 × 2 字节 = 12288 字节 ≈ 12KB
1024 token ≈ 12MB
2048 token ≈ 24MB
```

上下文翻倍只增加约 12MB KV。4GB 板上，这不是 OOM 的主要来源。

真正占内存的是权重、RKLLM 运行时工作区、内核和常驻服务。加载完成后、上下文跑满后再各读一次 `/proc/meminfo` 的 `MemAvailable`，用这个数决定 2048 能不能当正式版。

压缩或切换模型时，先销毁当前 RKLLM 句柄，再创建新句柄。两个句柄叠在一起会把权重量级的内存占两份，2048 压力测试会在这种峰值上失败，稳态其实可能是安全的。

### 1024 够不够用

1024 把 system、ChatML 标记、历史和本次回答算在一起。0.5B 的官方 system 很短，标记开销不大，多轮中文仍然很快顶到上限。所以自动压缩是正式功能，不是兜底。

压缩水位用运行时回报的 token 数，不用 `strlen/4` 估算。建议历史占到 max_context 的 70% 就压，给这一轮回答留余量。删掉一轮之后仍然超，就继续删最早的 user/assistant，直到低于水位。system 始终保留。

压缩之后重建会话，再把保留下来的历史整段 prefill 进去。RKLLM 没有稳定的「删掉最早一段 KV」接口；即便某个版本能清空全部 KV，也仍然要整段重放。重建时会有一次停顿，1024 上可以接受。

ChatML 用 C 字符串按官方模板拼接：

```text
<|im_start|>system
<官方 system 原文><|im_end|>
<|im_start|>user
...<|im_end|>
<|im_start|>assistant
```

调用时走原始 prompt 接口。运行时若再套一层 chat template，特殊标记会重复，输出会乱。以头文件里的输入类型为准，确认这一次推理不会二次包模板。

### 2048 怎么定

压力测试看三件事：加载后 `MemAvailable`、跑满上下文后的 `MemAvailable`、连续多轮压缩重建是否回得去（有没有把句柄、回调缓冲、LVGL 文本泄漏掉）。出现 OOM、NPU 分配失败、或压缩之后可用内存单调下降，正式版锁定 1024。

单帧显示 buffer 大约 3.5MB（720×1280×4），不参与这个判断。

### 速度

RK3576 NPU 的公开峰值是 6 TOPS（INT8）。W4A16 的解码速度要低于用这个峰值去除以参数量得到的数字。UI 上的 token/s 用 RKLLM 回调里的 token 计数除以墙钟时间，在板上测。0.5B 这一档可以交互，具体数字不写进计划。

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

这是主路径，和本树的 NPU 驱动匹配，风险在 API 用法和会话生命周期，不在模型能不能转换。

1. PC 上用与板端 `librkllm.so` 主版本一致的 RKLLM-Toolkit 转换。版本不一致是加载失败的常见原因。
2. 封装三件事：加载、建会话、流式推理。回调里累计 token 和时间，得到 token/s。
3. 消息列表放在进程内存。拼好的 prompt 是一段普通字符串。
4. 自动压缩和手动压缩走同一函数：保留 system，从最早一轮 user/assistant 开始删，然后销毁并重建会话，再 prefill。
5. 内存保护：周期看 `MemAvailable`。低于阈值（建议先用 200MB，以实测曲线改）时 UI 告警，并调用同一个压缩函数。压缩函数要能重入保护，避免告警和自动水位同时压两次。

回调里禁止：画 LVGL、写 socket、写 GATT、做压缩。回调只把增量文本推进一个有界队列。

模式切换和用户点停止时调用 RKLLM 的 abort，等这一轮回调结束再销毁句柄。

阶段出口：1024 模型多轮对话，自动压缩和手动压缩都能继续下一轮，`MemAvailable` 在压缩后回升，进程不退出。

---

## 第三阶段：网络

WiFi 管理沿用 `wpa_supplicant`。独立监控里看 `wpa_state`，断开再拉起，避免在已连接时反复重配。UI 显示地址、信号（`wpa_cli signal_poll` 或 nl80211）、连接状态。

WebSocket 用 libwebsockets，只听局域网地址上的一个端口。每个客户端一块发送队列。写失败、对端关闭，只回收这个连接。慢客户端队列满了就丢掉该客户端的旧 token 或关掉它，推理线程继续跑。

云端 GLM-4.7 用 libcurl 读 SSE。超时、HTTP 错误、中途断流，在 UI 上显示状态，并结束这一轮，本地屏和已经连上的 WebSocket 收到「本轮结束」而不是卡死。

API Key 只放进程内存，界面输入，进程退出即消失。不写 eMMC、不写配置文件、不进日志。原清单里的「联网拉取 API Key」和「界面输入」二选一，本方案用界面输入。联网拉取要多一个密钥来源和常驻客户端，不进这五阶段。

UI 切换「本地 Qwen2.5-0.5B / 云端 GLM-4.7」时，先停掉正在生成的那一路，再改模式。两条流不能同时往对话框里写。

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
| 局域网 HTTP 可用 WebBLE，公网 HTTP 不可用 | 浏览器 WebBLE 要 HTTPS 或 localhost。局域网 HTTP 同样不可用。远处手机走 WebSocket |
| 2048 必须深度裁剪才有希望，否则 OOM | 2048 的 KV 只比 1024 多约 12MB。用压力测试决定默认值。压缩时禁止两个 RKLLM 句柄并存 |
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
2. 1024：连续多轮触发自动压缩，再手动压缩一次。压缩后还能回答，`MemAvailable` 不单调下降，无内核 OOM。
3. 2048：换测试模型重复第 2 项。失败则正式版保持 1024，记录失败时的 `MemAvailable` 和 dmesg。
4. 模式切换：本地与 GLM-4.7 各完成一轮流式输出。切换发生在生成中途时，旧的一路停止，对话框只有一路文本。断网时云端路径给出错误状态，进程仍在。
5. 局域网 WebSocket：浏览器收全量流式文本。拔掉客户端，本地屏继续把这一轮显示完。
6. BLE：手机在旁边，写特征提问，Notify 能收到后续 token。关掉手机蓝牙，本地屏和 WebSocket 不受影响。浏览器路径另在 HTTPS 页面上复测。
7. frpc：默认开机没有 frpc 进程。UI 打开后，蜂窝流量能完成 WebSocket 对话。UI 关闭后进程消失，`MemAvailable` 回升。

第 1 项不过，不进模型。第 2 项不过，不开 WebSocket。第 5 项不过，不做 frpc。
