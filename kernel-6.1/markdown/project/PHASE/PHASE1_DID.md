# PHASE1 已做记录

> 对照 [PHASE1.md](./PHASE1.md)。只记已经落地的改动；未做步骤标「未做」。

## 进度总览

| PHASE1 步骤块 | 状态 |
|---|---|
| 1. 屏进板级 DTS | **已做**（脚位未对照原理图，沿用 dtsi 默认） |
| 2. 第一次内核配置 | **已做** |
| 3. 点亮 | **已过**（板上背光亮、MIPI 画面正常） |
| 4. 触摸 | **已过**（probe 成功；修坐标后指哪点哪；暂保持竖屏） |
| 5. WiFi / 蓝牙 | 未做 |
| 6. rootfs 裁剪 | 未做（建议屏/触摸稳定后再做） |
| 7. `llm_ctrl.ko` | 未做 |
| 8. LVGL 面板 | 未做 |
| 9. 收口 | 未做 |

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

---

## 驱动侧结论（截至本记录）

- **已过**: MIPI 点亮、FTS 触摸坐标正确（竖屏）。
- **未写**: `llm_ctrl.ko`。
- **下一步**: WiFi/蓝牙固件与连网（PHASE1 步骤 5），再 rootfs 裁剪；横屏暂不做。

---

## 工作区维护（非业务）

1. `git reset --hard HEAD` 清误脏  
2. 本仓 `core.filemode=false`  
3. macOS 大小写冲突头文件 `skip-worktree`  
