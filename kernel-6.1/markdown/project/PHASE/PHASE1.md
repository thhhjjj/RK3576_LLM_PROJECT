# 第一阶段：做到哪一步

> 依据 [project.md](../project.md) 的系统层。这一阶段结束时，板子是一台能看指标、能点屏幕的本机终端。模型、WebSocket、GATT、frpc 都还没开始。

## 完成线

上电后同时满足下面六条，才算第一阶段完成，才允许进第二阶段：

1. MIPI 稳定显示 720×1280，背光亮。
2. 手指点屏幕，LVGL 控件有按下和松开。
3. `wlan0` 关联完成并拿到 IPv4。判断用 `wpa_cli status` 里的 `wpa_state=COMPLETED`。
4. `hci0` 存在。蓝牙业务不做。
5. `llm_ctrl.ko` 开机即在，sysfs 读到的 NPU 温度在室温下合理（大约几十摄氏度），超温 netlink 能被用户态收到一次。
6. LVGL 上内存、CPU、NPU 负载、NPU 温度、GPU 负载、上下行网速都是采样值，约 1 秒刷新一次。空载 NPU 接近 0，断网时网速为 0。

对话区、压缩按钮、模式开关在屏幕上能看见、能点。点下去只改变按钮状态，不加载模型、不发网络请求。

上下文占用和 token/s 只留标签，显示 `—`。这两个数要等第二阶段有推理之后才有来源。

## 四块各自停在哪

| 块 | 做到这个程度 | 这一阶段停在这里 |
|---|---|---|
| 屏和触摸 | 打开 5 寸屏 include，画面是 720×1280，触摸坐标进到 LVGL | 不写拼音输入法，对话框不能在本地打字 |
| WiFi / 蓝牙固件 | 能上网，`hci0` 出现，固件在 rootfs 里 | 不做重连策略、不做 GATT、不做 WebSocket |
| 裁剪 | 裁完上面六条仍然成立，并记下当时的 `MemAvailable` 当作第二阶段的基线 | 不把 CMA 预先改小，不靠单帧 buffer 去给模型腾内存 |
| `llm_ctrl.ko` + LVGL | 温度可读、告警能到用户态、六项指标在刷 | 告警只显示在界面上，不触发压缩，不关 NPU |

## 屏和触摸

面板用本树现成的 [rk3576-kickpi-lcd-mipi-5-720-1280-F050008M01.dtsi](../../../arch/arm64/boot/dts/rockchip/rk3576-kickpi-lcd-mipi-5-720-1280-F050008M01.dtsi)。[rk3576-kickpi-k7c.dtsi](../../../arch/arm64/boot/dts/rockchip/rk3576-kickpi-k7c.dtsi) 第 19 行的 include 当前是注释，打开它。

这个文件里已经写好的内容，第一阶段直接用：

- 分辨率 720×1280，4 lane，RGB888，像素时钟 84MHz
- DSI 接到 VP1（`&dsi_in_vp1` 为 okay，VP0 留给现有 HDMI）
- 复位 `gpio0 RK_PA2`，背光 `pwm1_6ch_1`，电源 `gpio0 RK_PC6`
- 触摸 `focaltech,fts` 在 `i2c0` 地址 `0x38`，坐标范围就是 `0 0 720 1280`，状态是 okay。同文件里的 GT9xx 是 disabled

时序和初始化序列不改。复位、背光、电源三只脚如果和原理图不一致，只改脚，不改 `hactive` / `vactive`。

`rockchip_linux_defconfig` 里有 `CONFIG_TOUCHSCREEN_GT9XX`，没有 `CONFIG_TOUCHSCREEN_FTS`。面板节点启用的是 FocalTech，所以第一阶段要打开 `CONFIG_TOUCHSCREEN_FTS`，让 `/dev/input/event*` 出现这颗触摸，LVGL 读这个 event 设备。点一个按钮能看到按下和松开，即算触摸完成。

NPU 节点不用在这一阶段新写。K7C 包含的 [rk3576-kickpi-evb.dtsi](../../../arch/arm64/boot/dts/rockchip/rk3576-kickpi-evb.dtsi) 已经把 `&rknpu` 和 `&rknpu_mmu` 设为 okay。

## WiFi 和蓝牙

本机实测是 **Seekwave SWT6621S**（SDIO `1ffe:6621`，`swt6621s_wifi.ko`），不是 K7C 设备树里的 AP6256。WiFi 和蓝牙都走 SDIO，`hci0` 的 `Bus` 是 SDIO。记录在 [PHASE1_DID.md](./PHASE1_DID.md) §6。

树里的 [rk3576-kickpi-k7c-wifi.dtsi](../../../arch/arm64/boot/dts/rockchip/rk3576-kickpi-k7c-wifi.dtsi) 仍描述博通 `bcmdhd` 和 UART4 蓝牙，不要按那个去换驱动或下载 `BCM4345C5.hcd`。

完成的样子：

- `wpa_cli status` 为 `wpa_state=COMPLETED`
- `ip addr` 里 `wlan0` 有 IPv4，默认路由能 `ping` 通网关
- `hciconfig` 能看到 `hci0`
- 重启后不用手工再执行 `wpa_supplicant` / `dhclient`

`LOWER_UP` 不算连上 AP。Debian 上用 `/etc/wpa_supplicant.conf` 和 `wlan0-connect.service` 验证过即可；正式 rootfs 改刷裁剪后的 Buildroot，wpa 配置照搬，启动方式改走 Buildroot 的 init（无 systemd 服务文件）。

## 裁剪

裁剪的完成标准是「六条完成线都还在」，外加一条基线记录：系统空闲、WiFi 已连接、LVGL 已显示时，把 `/proc/meminfo` 的 `MemAvailable` 记下来。第二阶段用这个数看模型吃掉了多少。

必须仍然开着：

- `CONFIG_ROCKCHIP_RKNPU`，以及板级已经 okay 的 NPU 和 IOMMU
- DRM、Rockchip 显示、MIPI DSI、`simple-panel-dsi`
- SDIO，以及本机的 `swt6621s_wifi.ko`
- 能带起 `hci0` 的 Seekwave 初始化（不是 `brcm_patchram_plus1`）
- 打开 `CONFIG_ROCKCHIP_RKNPU_PROC_FS`，使 `/proc/rknpu/load` 存在。UI 读这个文件，不读 debugfs

rootfs 里留下后面阶段要用、但这一阶段可以先不调用的包：`wpa_supplicant`、`bluez5`、`libdrm`、`librkllm`、`libwebsockets`、`libcurl`（带 CA 证书）。删掉它们不算裁剪完成，算把后阶段的门焊死。

CMA 保持 SDK 默认。单帧 dumb buffer（720×1280×4 ≈ 3.5MB）可以做，有撕裂也接受。它不是本阶段的通过条件。

## `llm_ctrl.ko`

开机加载的 platform 驱动。温度从类型为 `npu-thermal` 的 thermal zone 读取，单位沿用内核的毫摄氏度，sysfs 给用户态时可以换成摄氏度，但要和 LVGL 显示一致。

完成的样子：

- 模块在启动脚本或 `modules-load` 里，重启后不用手工 `insmod`
- 读 sysfs，数值随时间变化，室温下落在几十摄氏度，而不是 0 或 115000 这种未换算的原始值直接当摄氏度显示
- 告警阈值正式用 85°C 或 95°C，低于内核 critical 115°C
- 验收时把阈值临时改到低于当前温度，用户态收到一次 netlink，界面出现超温提示；测完改回正式阈值

驱动不映射 TSADC，不注册第二个传感器。115°C 关机仍由内核 thermal 做。

## LVGL

DRM 全屏，画布 720×1280 竖屏。指标在 LVGL 定时器里读，不另开线程画控件。

| 显示项 | 数据从哪来 | 什么样算对 |
|---|---|---|
| 内存 | `/proc/meminfo` 的 `MemTotal`、`MemAvailable` | 已用加可用与总内存同一量级，可用值大于 0 |
| CPU | `/proc/stat` 相邻两次做差 | 空闲时个位数到几十，滑一下屏幕会上升 |
| NPU 负载 | `/proc/rknpu/load` | 这一阶段没有推理，显示接近 0 |
| NPU 温度 | `llm_ctrl` 的 sysfs | 与命令行读到的温度一致 |
| GPU 负载 | `/sys/class/devfreq/` 下 GPU 节点的 `load`，节点名在板上确认后写死到程序里 | 有刷新，不全是空字符串 |
| 上行 / 下行 | `/proc/net/dev` 里 `wlan0` 相邻两次做差 | 断网为 0；`ping` 时下行升高 |
| 上下文、token/s | 无 | 固定显示 `—` |
| 对话框、压缩、本地/云端开关 | 无数据 | 可见、可点，点击不产生输出 |

中文字体用子集，能画出指标标题和按钮文字即可。

## 不过线就不进第二阶段

对应 [project.md](../project.md) 验收第 1 条。下面任意一条成立，第一阶段就不算完成：

- 分辨率不是 720×1280，或背光不亮
- 触摸没有 event 设备，或点按钮没有反应
- 只有 `LOWER_UP`、没有 `wpa_state=COMPLETED`，或没有 IPv4
- 没有 `hci0`
- 温度读出来是 0，或 netlink 用临时低阈值也收不到
- 六项指标里有一项长期空白，或断网时网速不是 0
- 为了界面去加载 `.rkllm`、打开监听端口、注册 GATT


## 细分步骤

按这个顺序做。内核配置在第一次刷机前改完，点亮之后不再为触摸或 NPU 负载重编内核。每一步过了再做下一步。

### 1. 屏进板级 DTS

1. 打开 [rk3576-kickpi-k7c.dtsi](../../../arch/arm64/boot/dts/rockchip/rk3576-kickpi-k7c.dtsi) 第 19 行，include `rk3576-kickpi-lcd-mipi-5-720-1280-F050008M01.dtsi`。
2. 对照原理图看三只脚：复位 `gpio0 RK_PA2`、背光 `pwm1_6ch_1`、电源 `gpio0 RK_PC6`。不一致就只改这三处。
3. 确认没改 `hactive`、`vactive`、lane 数、像素时钟和 `panel-init-sequence`。
4. 确认 DSI 仍走 VP1，HDMI 仍走 VP0。

### 2. 第一次内核配置

5. 打开 `CONFIG_TOUCHSCREEN_FTS`。面板节点用的是 `focaltech,fts`，现有 defconfig 里只有 GT9xx。
6. 打开 `CONFIG_ROCKCHIP_RKNPU_PROC_FS`，准备好 `/proc/rknpu/load`。
7. 核对仍然开着：`CONFIG_ROCKCHIP_RKNPU`、DRM、MIPI DSI、`simple-panel-dsi`、SDIO、rfkill。`CONFIG_BT_HCIUART` 保持关闭。
8. CMA 保持 SDK 默认值，这一步不改。
9. 编内核和 dtb 并刷入。

### 3. 点亮

10. 上电看 dmesg：DSI、面板没有反复 reset。
11. `modetest -M rockchip` 里这块屏的 mode 是 720×1280。
12. `/sys/class/backlight/` 下对应节点的 `brightness` 大于 0，屏幕亮着。
13. 这一步只要求有稳定画面。LVGL 还没上，停在控制台或纯色都算过。

### 4. 触摸

14. dmesg 里 FocalTech 在 `i2c0` 地址 `0x38` probe 成功。
15. `/proc/bus/input/devices` 里能找到这颗触摸对应的 `event` 节点。
16. `evtest` 点一下：`ABS_X` 落在 0–719，`ABS_Y` 落在 0–1279。

### 5. WiFi 和蓝牙通上

17. 确认 Debian rootfs 里已有 Seekwave 驱动。`wifibt-init.sh start` 应打印 `Seekwave` / `1ffe:6621` / `swt6621s_wifi.ko`，并出现 `wlan0`。不要改去加载 `bcmdhd` 或拷 AP6256 的三份博通固件。
18. 用 `wpa_cli scan_results` 里的 SSID 原文写 `/etc/wpa_supplicant.conf`。WPA2+WPA3 混合热点写 `key_mgmt=WPA-PSK SAE`。
19. 用 `wlan0-connect.service` 在 `wlan0` 出现后再启动 `wpa_supplicant` 和 `dhclient`。断线后的重连策略留到第三阶段。
20. `wpa_cli status` 为 `wpa_state=COMPLETED`，`wlan0` 有 IPv4，`ping` 通网关。
21. `hciconfig` 能看到 `hci0`，且 `Bus: SDIO`。不做配对、不做 GATT。
22. 重启再查一次第 20、21 步，确认不用手工 `insmod`、不用手工敲连接命令。

### 6. rootfs 裁剪（改刷 Buildroot，不在 Debian 上 apt 删）

23. lunch `rockchip_rk3576_kickpi_k7c_buildroot_defconfig`。它把 `RK_BUILDROOT_BASE_CFG` 指到裁剪版 `rockchip_rk3576_kickpi_k7c_llm_defconfig`：去掉 Chromium / Weston / GStreamer / camera / audio / 压测套件，留下 `wpa_supplicant`、`bluez5`、`rkwifibt`（Seekwave）、`libdrm`、`libwebsockets`、`libcurl`、CA、`rknpu2`。`librkllm` 本树没有 Buildroot 包，第二阶段用 overlay 塞。
24. `./build.sh rootfs`（或 `build.sh all` 里只取 rootfs）。只刷 rootfs，不刷 boot/U-Boot。把 Debian 上已验证的 `/etc/wpa_supplicant.conf` 拷进 Buildroot（Buildroot 无 systemd，用已有 `wifibt-init` / 启动脚本起 `wpa_supplicant` + `dhcpcd`）。
25. 重复第 11、12、16、20、21 步：画面、触摸、WiFi、`hci0` 都还在。确认没有改过 CMA。

### 7. `llm_ctrl.ko`

26. 在 shell 里找到 `type` 为 `npu-thermal` 的 thermal zone，读 `temp`。确认单位是毫摄氏度，室温大约几万。
27. 写 platform 驱动：只读这个 thermal zone，sysfs 输出摄氏度。不映射 TSADC。
28. 用 genetlink 发超温事件。正式阈值写 85°C 或 95°C。
29. 模块放进开机加载。重启后 `lsmod` 里有它，不用手工 `insmod`。
30. sysfs 的摄氏度和 `temp / 1000` 一致。
31. 把阈值临时改到低于当前温度，用户态收包程序收到一次事件。测完改回正式阈值。

### 8. LVGL 面板

32. 采样先做成独立函数，打印到串口：内存、CPU、NPU 负载、NPU 温度、GPU 负载、`wlan0` 上下行。数值符合上面的「什么样算对」之后，再绑到控件。
33. GPU 节点在板上从 `/sys/class/devfreq/` 确认名字，写进程序。
34. LVGL 用 DRM 全屏 720×1280，输入设备用第 15 步的 event 节点。采集放在 LVGL 定时器里，大约 1 秒一次。
35. 画出六项指标。空载时 NPU 负载接近 0；停掉 WiFi 后上下行回到 0，再连上后 `ping` 时下行升高。
36. 画出对话框、压缩按钮、本地/云端开关。点击只切换按钮状态，不加载模型、不打开端口。
37. 上下文占用和 token/s 两个标签固定显示 `—`。
38. 第 31 步的 netlink 接到界面上，显示一条超温提示。提示出现之后不关 NPU、不触发压缩。
39. 中文字体用子集，指标标题和按钮文字能显示出来。

### 9. 收口

40. 系统空闲、WiFi 已连接、LVGL 已显示时，记下 `/proc/meminfo` 的 `MemAvailable`，作为第二阶段的基线。
41. 重启。六条完成线全部成立，并且没有为了界面去加载 `.rkllm`、监听端口或注册 GATT。

