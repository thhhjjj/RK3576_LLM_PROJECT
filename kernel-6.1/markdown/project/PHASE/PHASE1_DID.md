# PHASE1 已做记录

> 对照 [PHASE1.md](./PHASE1.md)。只记已经落地的改动；未做步骤标「未做」。

## 进度总览

| PHASE1 步骤块 | 状态 |
|---|---|
| 1. 屏进板级 DTS | **已做**（脚位未对照原理图，沿用 dtsi 默认） |
| 2. 第一次内核配置 | **已做** |
| 3. 点亮 | **已过**（板上背光亮、MIPI 画面正常） |
| 4. 触摸 | **已过**（probe 成功；修坐标后指哪点哪；暂保持竖屏） |
| 5. WiFi / 蓝牙 | **已过**。芯片是 Seekwave SWT6621S（SDIO `1ffe:6621`，`swt6621s_wifi.ko`），不是 AP6256。重启后 `wpa_state=COMPLETED`，`wlan0` 为 `192.168.31.89`，`hci0` `Bus: SDIO` 且 `UP`。见下文 §6 / G–I |
| 6. rootfs 裁剪 | **已过**。裁剪 Buildroot 已刷；屏/触摸仍在；WiFi/BT 重启保持。LVGL 已起后需再记一次 `MemAvailable` 作第二阶段基线（步骤 9） |
| 7. `llm_ctrl.ko` | **已过**。`thermal_zone4`/`npu-thermal`；sysfs `temp_c`；genetlink `OVERTEMP` 已测；`/lib/modules/llm_ctrl.ko` + `S39llm_ctrl` 开机自载 |
| 8. LVGL 面板 | **已过**。`llm_panel` 进 rootfs，`S90llm_panel` 自启；板上 **能显示、能触摸**（2026-10-17）。指标 1s 刷新 + Compress/模式开关可点（只改 UI 态）。**中文字体尚未做**（必要，标签仍英文）；富 UI 留后阶段 |
| 9. 收口 | 未做：记 LVGL 运行时 `MemAvailable`；中文字体子集；确认六项指标数值合理后关 PHASE1 |

另：K7C 无关 include 与 EVB 编解码等节点已关；Ubuntu 上只刷 `boot.img` 即可验证内核/DTS。

---

## Git 树（本地完整 SDK vs 云端轻量仓）

### 本地 `main`（完整 SDK 仓）

| commit | 说明 |
|---|---|
| `438c83c8c` | DTS 开 5 寸 MIPI + 裁剪 K7C/EVB；defconfig 开 FTS + RKNPU_PROC_FS |
| `2d3c20de1` | 设备树裁剪编译版本：FTS 适配 6.1（`kernel_read`/`write`、Makefile 去掉 `focaltech_test/`）+ PHASE 文档 |
| `90249bba9` | 触摸坐标：恢复 DT `display-coords`，默认改 720×1280 |
| `ed37ff028` | 增加 `scripts/git_push_must_change.sh` |
| `b9e8597cc` | K7C 裁剪 Buildroot defconfig + WiFi/BT 记录并入 PHASE1_DID |
| `90be4d8d0` | llm驱动版本：`llm_ctrl` 外置模块 + PHASE1_DID |

### 云端 `origin/main`（https://github.com/thhhjjj/RK3576_LLM_PROJECT）

历史与本地 SDK **无关**；只存必须改动的文件，非整库。

| commit | 说明 |
|---|---|
| `89b27f2d5` | Initial commit（`.gitignore`） |
| `5b45866c6` | 三个文件：k7c/evb dtsi + defconfig |
| `c9ff0feae` | FTS `focaltech_core.c` / `.h`（坐标修复） |
| `9a119e90e` | `git_push_must_change.sh`（根目录 + `scripts/`） |

当前云端文件列表：

```text
.gitignore
git_push_must_change.sh
scripts/git_push_must_change.sh
kernel-6.1/arch/arm64/boot/dts/rockchip/rk3576-kickpi-evb.dtsi
kernel-6.1/arch/arm64/boot/dts/rockchip/rk3576-kickpi-k7c.dtsi
kernel-6.1/arch/arm64/configs/rockchip_linux_defconfig
kernel-6.1/drivers/input/touchscreen/focaltech_touch/focaltech_core.c
kernel-6.1/drivers/input/touchscreen/focaltech_touch/focaltech_core.h
```

> 说明：本地还有 FTS 编译修复（flash/test/Makefile）在 `2d3c20de1`，曾通过 `makediff` 拷到 Ubuntu 编过；尚未全部镜像进云端轻量仓。需要时用 `makediff` 或再推一轮补齐。

### 同步到 Ubuntu SDK

```bash
# 在 SDK 根目录
./scripts/git_push_must_change.sh
# 或指定已有 overlay（如 makediff）
./scripts/git_push_must_change.sh /path/to/makediff
```

旧文件进 `lastonce/<相对路径>`；只刷 `output/firmware/boot.img`（或 `kernel/boot.img`）即可验内核改动。

---

## 改动明细

### 1. `rk3576-kickpi-k7c.dtsi`

| 操作 | 内容 |
|---|---|
| 打开 | `#include "rk3576-kickpi-lcd-mipi-5-720-1280-F050008M01.dtsi"` |
| 注释掉 | `lcd-dp`、`ethernet-gmac0`、`combopyh0-pcie`、`k7c-extend-40pin`、`ir`、`k7c-cam0/1/3` |
| 保留 | HDMI、5 寸 MIPI、`k7c-wifi.dtsi` |
| 未改 | 时序 / 脚位默认 / DSI→VP1、HDMI→VP0 |

### 2. `rk3576-kickpi-evb.dtsi`

关掉：`adc_keys`、`hdmi_sound`、`sai6`、`PCIE_PWREN`、`combphy0_ps`、`usbdp_phy_dp`、`test-power`、`crypto`、`iep*`、`jpeg*`、`mpp_srv`、`rga2*`、`rkvenc*`、`rkvdec*`、`vdpp`。

保留：`rknpu*`、`gpu`、`vop*`、`tsadc`、`sdhci`、USB、电源、RTC 等。

### 3. `rockchip_linux_defconfig`

| 项 | 改动 |
|---|---|
| 触摸 | `CONFIG_TOUCHSCREEN_FTS=y`；关掉 `FT8756`（避免双 probe `focaltech,fts`） |
| NPU 负载 | `CONFIG_ROCKCHIP_RKNPU_PROC_FS=y` |
| 未改 | `CONFIG_ROCKCHIP_RKNPU`、CMA、`BT_HCIUART` 保持关 |

### 4. FTS 驱动（编译 + 坐标）

| 文件 | 改动 |
|---|---|
| `focaltech_flash.c` / `focaltech_test*.c` | 去掉已删的 `set_fs`/`KERNEL_DS`，改 `kernel_read`/`kernel_write` |
| `Makefile` | 去掉 `focaltech-ts-y += focaltech_test/`（6.1 会把目录塞进 `vmlinux.a`） |
| `focaltech_core.c` | 恢复解析 DT `focaltech,display-coords` |
| `focaltech_core.h` | 默认分辨率改为 720×1280（原 1600×2176） |

### 5. 工具脚本

`scripts/git_push_must_change.sh`：拉取轻量仓（或本地 overlay）→ 备份到 `lastonce/` → 按路径拷进 SDK。

### 6. WiFi / 蓝牙（Debian 上验证通过；未改内核）

板子当时仍是 Debian `linaro-alip`，只刷过裁剪后的 `boot.img`。没有重编内核，没有重刷 rootfs。

| 项 | 结论 |
|---|---|
| 芯片 | **Seekwave SWT6621S**，SDIO `1ffe:6621`，驱动 `swt6621s_wifi.ko` |
| 不是 | AP6256 / `bcmdhd` / `BCM4345C5.hcd`。树里的 `rk3576-kickpi-k7c-wifi.dtsi` 仍写博通，**不要按那个去换驱动或下博通固件** |
| WiFi | `/etc/wpa_supplicant.conf`；SSID 用 `scan_results` 的 **hex**（避免 `～`/`・`/空格打错）；`key_mgmt=WPA-PSK SAE`（家路由 WPA2+WPA3） |
| 开机连网 | systemd `wlan0-connect.service`：等 `wlan0` 出现后再起 `wpa_supplicant` + `dhclient` |
| 蓝牙 | `hci0`，`Bus: SDIO`，重启后自己 UP；不做配对/GATT |
| 验收 | 重启后 `wpa_state=COMPLETED`，`wlan0` 有 IPv4（曾为 `192.168.31.89`），`hci0` UP |

板端关键文件（仅 Debian，改刷 Buildroot 后启动方式要换）：

```text
/etc/wpa_supplicant.conf
/etc/systemd/system/wlan0-connect.service
```

### 7. Buildroot 裁剪（已刷、已复验）

决定：**不在 Debian 上 apt 删包**，改 lunch Buildroot，用裁剪版 defconfig 编 rootfs，Mac 上 `rkdeveloptool wlx rootfs` 只刷 rootfs。

| 文件 | 作用 |
|---|---|
| `device/rockchip/.chips/rk3576/rockchip_rk3576_kickpi_k7c_buildroot_defconfig` | `RK_BUILDROOT_BASE_CFG="rk3576_kickpi_k7c_llm"`，`RK_ROOTFS_SYSTEM=buildroot` |
| `buildroot/configs/rockchip_rk3576_kickpi_k7c_llm_defconfig` | 相对默认 `rockchip_rk3576_defconfig` 的瘦身入口 |
| `buildroot/configs/rockchip/products/llm_terminal.config` | 后阶段必留包 |

相对默认 rk3576 Buildroot：

| 去掉 | 留下 |
|---|---|
| Chromium、Weston、GStreamer、camera、audio、CAN/PCI、压测/rockchip-test、NTFS/exFAT、系统中文字体 | base、rk3576 aarch64、VFAT、`wifibt`（wpa + bluez + **rkwifibt**/Seekwave）、`rknpu2`、`libdrm`、`libcurl`+OpenSSL+CA、`libwebsockets`、少量 bring-up 工具（`evtest`/`iw` 等） |

`librkllm` 本树没有 Buildroot 包；PHASE1 不装、不加载。第二阶段用 overlay 塞；Qwen3.5-2B 须 RKLLM ≥ 1.3.0。

板端验收（2026-10-17）：

| 项 | 结果 |
|---|---|
| 系统 | `NAME=Buildroot`，hostname `rk3576-buildroot`（不再是 `linaro-alip`） |
| 屏 / 触摸 | 正常（boot 未重刷） |
| WiFi | 迁入 Debian 同款 hex SSID + `WPA-PSK SAE`；重启后 `COMPLETED`，`192.168.31.89`，`key_mgmt=SAE` |
| 蓝牙 | `hci0` `Bus: SDIO` `UP RUNNING` |
| 内存 | `MemAvailable: 3823600 kB`（尚无 LVGL；步骤 9 再记正式基线） |
| CMA | 未改 |

产物：`output/buildroot/images/rootfs.ext2`（≈860M）→ 拷成 `rootfs.img`；`rkdeveloptool`：`adb reboot loader` → `wlx rootfs` → `rd`。

### 8. `llm_ctrl.ko`（外置模块；已过）

源码：`kernel-6.1/markdown/project/llm_ctrl/`（commit `90be4d8d0` llm驱动版本）。

| 文件 | 作用 |
|---|---|
| `llm_ctrl.c` / `llm_ctrl.h` | platform 驱动：读 `npu-thermal`，sysfs 出摄氏度，超温 genetlink |
| `nl_watch.c` | 用户态听 `OVERTEMP`（静态链） |
| `Makefile` | `make modules` + `nl_watch`；`KDIR` 须与板载内核一致 |
| `buildroot/.../overlays/30-llm_ctrl/` | `S39llm_ctrl` + `prepare.sh`（编 rootfs 时若有 `.ko` 则装进 `/lib/modules/`） |

行为：

| 项 | 约定 |
|---|---|
| thermal | `thermal_zone_get_zone_by_name("npu-thermal")`（本机为 zone4） |
| sysfs | `/sys/devices/platform/llm_ctrl/temp_c`（只读）、`threshold_c`（可写，默认 85，须 &lt; 115） |
| genetlink | family `llm_ctrl`，组 `events`，命令 `OVERTEMP`；超阈发一次，降到阈值−2°C 后解除 |
| 不做什么 | 不映射 TSADC、不注册第二传感器、不关 NPU |

板端验收：

| 项 | 结果 |
|---|---|
| 温度 | `temp_c` 与 `zone4/temp / 1000` 一致（约 23–29°C） |
| 告警 | `echo 20 > threshold_c` 后 `nl_watch` 收到 `OVERTEMP`；测完改回 85 |
| 开机加载 | `/lib/modules/llm_ctrl.ko` + `/etc/init.d/S39llm_ctrl`；`adb reboot` 后 `lsmod` 有、`temp_c` 可读 |

日常装板（不重刷）：`adb push` `.ko` 与 `S39llm_ctrl` → `chmod` → `reboot`。下次编 rootfs 前把编好的 `llm_ctrl.ko` 放在源码目录，overlay 会拷进镜像。

---

## 问题怎么解决的（按时间线）

### A. 只推改动、不推 20GB SDK

本地 `main` 与 `origin/main` **没有共同祖先**。做法：在临时 worktree 检出 `origin/main`，只 `git checkout <本地提交> -- <文件>`，再快进推到云端 `main`。Graph 对不齐是预期，不是鉴权失败。

### B. 编译 1：`KERNEL_DS` undeclared

`CONFIG_TOUCHSCREEN_FTS` 的老代码仍用 5.10 已移除的 `set_fs`/`KERNEL_DS`。对照同树 `FT5726` / `FT8756`（有版本宏或已改 `kernel_read`），改成 `kernel_read`/`kernel_write`。

### C. 编译 2：`focaltech_test` is not an object

复合目标里写了 `focaltech-ts-y += focaltech_test/`，kbuild 6.1 把**目录名**链进 `vmlinux.a`。且 `FTS_TEST_EN=0`，工厂测试未启用 → 从 Makefile 去掉该行。

### D. 触摸能点但坐标不对

DTS 已写 `display-coords = <0 0 720 1280>`，但 `fts_get_dt_coords()` 里读 DT 的代码被注释，一直用默认 **1600×2176**。恢复 DT 解析并改默认值 → 指哪点哪。

### E. 产物与烧录

`./build.sh kernel` → `output/firmware/boot.img` 多为软链，真实文件常在 `kernel/boot.img` / `kernel-6.1/boot.img`。本次只改内核/DTS → **只刷 boot**，不必重刷 U-Boot/rootfs；不要单独刷 zImage。

### F. 工作区 / Graph

约一万个误脏文件（多为 `filemode`）拖死 IDE Graph → `reset --hard` + `core.filemode=false` + 部分 `skip-worktree`。

### G. WiFi：文档写 AP6256，板子是 Seekwave

`wifibt-init` / SDIO 枚举出来是 `1ffe:6621` + `swt6621s_wifi.ko`，不是 `02d0:a9bf` / `bcmdhd`。固件与驱动已在 Debian rootfs 里，**不要**按 `k7c-wifi.dtsi` 去换博通三件套。文档与 `project.md` / `wifi_device.md` 口径已改成 Seekwave。

### H. WiFi：`SCANNING` 连不上

SSID 含全角/空格时手打必错。用 `wpa_cli scan_results` 里的 **hex SSID** 写入 `ssid=`（不加引号）。家路由 WPA2+WPA3 → `key_mgmt=WPA-PSK SAE`。

### I. WiFi：重启后 `DISCONNECTED`

`wlan0-connect.service` 起太早，`wlan0` 还没出现。改成等接口再起 `wpa_supplicant`/`dhclient` 后，重启可 `COMPLETED`。

### J. rootfs：Debian 上裁 vs 改刷 Buildroot

PHASE1 步骤 6 要可复现瘦 rootfs，不在 `linaro-alip` 上 `apt remove`。已 lunch 裁剪版 → 编 rootfs → 只刷 rootfs → 迁 wpa → 复验通过。

### K. 刷机：系统在跑时 `rkdeveloptool ld` 找不到设备

正常系统只给 `adb`。先 `adb reboot loader`，再 `ld` 见 `Loader`，才能 `wlx`。

### L. Buildroot 上 `psk` 引号少一边

heredoc 粘贴时写成 `psk=07210d000721"` → parse 失败。两边都要有 `"`。

### M. `nl_watch`：`NLA_*` 链接失败

用户态 glibc 头没有内核那套 `NLA_OK`/`NLA_NEXT` 宏，被当成函数 → undefined reference。在 `nl_watch.c` 里补本地宏。

### N. `adb` 只能在 Mac 跑

板子 shell 里没有 `adb`。先 `exit` 再 `adb push`；进板子后只跑 `insmod`/`lsmod`。

### O. 开机加载误判失败

`S39` 已在约 4s 加载（dmesg），但 `adb shell` 与后面的 `lsmod` 分行粘贴时，`lsmod` 可能跑在 Mac 上得到空。用一行：`adb shell 'lsmod | grep llm_ctrl; cat .../temp_c'`。

---

## 驱动侧结论（截至本记录）

- **已过**: MIPI、FTS、Seekwave、裁剪 Buildroot、`llm_ctrl`（sysfs + genetlink + 开机自动加载）。
- **未写**: LVGL。
- **下一步**: PHASE1 步骤 8 LVGL；横屏暂不做。正式 `MemAvailable` 基线等 LVGL 起来后再记。

---

## 工作区维护（非业务）

1. `git reset --hard HEAD` 清误脏  
2. 本仓 `core.filemode=false`  
3. macOS 大小写冲突头文件 `skip-worktree`  
