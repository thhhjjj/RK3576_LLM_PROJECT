# PHASE6 已做记录

> 对照 [PHASE6.md](./PHASE6.md)。只记落地项。

## 进度总览

| 步骤 | 状态 |
|---|---|
| 1. 评估 DRM/Mali/EGL | **已过（2026-10-24）** — 用户态进 llm rootfs 并刷机 |
| 2. GPU 显示后端接入 | **已过（0.3.0 板验 OK）** — 含 0.3.1 swizzle |
| 2b. 约 60fps（PHASE7-E 提前） | **已过** — 0.4.2 屏上约 **61 fps**（0.4.1 曾 39，双重 vsync） |
| 3. 验收对比表 + 回退 | **未做**（可选：dumb 对照） |

---

## 基线（代码阅读，lv_drivers 8.3.0）

路径：`buildroot/dl/lv_drivers/…/display/drm.c` → `drm_disp_drv_init`（`main.c`）。

| 项 | 结论 |
|---|---|
| 打开设备 | `DRM_CARD` / 默认 card；要求 `DRM_CAP_DUMB_BUFFER` |
| Atomic | `DRM_CLIENT_CAP_ATOMIC` |
| 缓冲 | `drm_allocate_dumb` × **2**；`drm_flush` 用 `cur_bufs` 翻页 + partial 时 memcpy 旧缓冲 |
| 绘制 | LVGL 把脏区 `color_p` **CPU memcpy** 进 dumb `map` |
| GPU | **未参与画 UI**；指标里的 GPU% 来自 `devfreq .../load`（`metrics.c`） |

含义：PHASE6 不是「从单 FB 改双 FB」这么简单，而是 **换绘制器**；双 dumb 已有，可作 GPU 失败时的回退。

---

## 板上探查口令（插上 adb 后跑）

```bash
adb shell 'ls -l /dev/dri; ls /usr/lib/libmali* /usr/lib/libEGL* /usr/lib/libgbm* /usr/lib/libGLESv2* 2>/dev/null; ls /sys/class/devfreq; for d in /sys/class/devfreq/*; do echo "== $d"; cat $d/name 2>/dev/null; done'
```

### 板探结果（刷机前，2026-10-24）

```text
/dev/dri: card0, card1, renderD128, renderD129
/usr/lib/libmali* libEGL* … → 无
devfreq: 27800000.gpu ← 内核 GPU 在
```

### 刷机后验证（2026-10-24）

```text
rkdeveloptool wlx rootfs …/rootfs.ext4 → Write LBA 100%
adb shell ls:
  /usr/lib/libEGL.so.1          5.6K
  /usr/lib/libmali.so.1.9.0    41.3M
```

| 结论 | 含义 |
|---|---|
| 内核侧 GPU **在** | `renderD12x` + `27800000.gpu` |
| 用户态 mali/EGL **已在 rootfs** | 可做 EGL 冒烟 / 再接 LVGL |
| LVGL 绘制 | **仍** DRM dumb（默认 + 回退） |

---

## Buildroot / SDK 依赖

| 候选 | 有无 | 备注 |
|---|---|---|
| libmali / EGL / gbm stubs | **板上有**（mali_autoload → prepare） | wayland-gbm G52 |
| LVGL GPU flush | **未接** | 等 probe 过后再换后端 |
| DRM dumb（现状） | **有** | 继续默认与回退 |

---

## 后端选型

- [x] A：GBM + EGL + GLES ← **目标**（blob 已刷上）  
- [ ] B：SDK LVGL GPU / RGA / 2.5D  
- [x] 用户态 blob：`makediff/mali_autoload/`（含 wayland c/s）→ prepare / 可 adb 快测

---

## 改动明细（落地）

| 文件 | 内容 |
|---|---|
| `llm_panel/gpu_probe.c` | 早期冒烟（仍保留）；`hal` 已改走 `gpu_disp` |
| `llm_panel/gpu_disp.c` / `.h` | LVGL → GLES 纹理 → GBM swap → DRM SetCrtc；`LLM_PANEL_FORCE_DUMB=1` 回退 |
| `llm_panel/main.c` | `hal_init`：先 `gpu_disp_init`，失败 `drm_disp_drv_init` |
| `llm_panel/CMakeLists.txt` | `gpu_probe.c` + `gpu_disp.c` |
| `llm_panel/ui.c` | `vtest：0.4.2`；右上角帧率 |
| `main`/`gpu_disp` | 0.4.2：去掉 vsync 双重等待（0.4.1 实测 ~39fps） |
| `llm_panel/gpu_disp.c` | PageFlip + last-flush present；refr 16ms；`gpu_disp_fps()` |
| `llm_panel/main.c` | GPU 路径每圈 `invalidate` + sleep≤16ms |

### 板上验收 probe（0.2.2 + wayland）

首次失败：缺 wayland。补库并 `adb push` 后：

```text
gpu_probe: opened /dev/dri/renderD128
gpu_probe: EGL 1.4
gpu_probe: OK (GLES2 context current, no surf)
llm_panel: GPU probe OK (still DRM dumb for LVGL)
arm_release_ver: g13p0-01eac0, rk_so_ver: 10
```

### 补库（已整合）

`Desktop/mali_autoload` → **`makediff/mali_autoload/`**（mali + EGL stubs + wayland c/s 0.22.0）。  
`prepare.sh` / `git_push` 拷 `libwayland*.so*`。固化：`git_push` → `./build.sh rootfs`（否则重启丢 wayland）。

### 0.3.0 板验（2026-10-24）— **通过**

```text
gpu_disp: /dev/dri/card0 connector 196 720x1280
gpu_disp: crtc 100
gpu_disp: EGL 1.4
gpu_disp: tex format RGBA (no BGRA_EXT)
gpu_disp: OK 720x1280 GLES texture → GBM → DRM (LVGL full_refresh)
llm_panel: display=GPU (GBM/EGL)
```

### 0.3.1

片元 `c.bgra` swizzle（无 BGRA_EXT）。若色仍反，再调。

### 0.4.0 — PHASE7-E 60fps 提前并入

| 手段 | 说明 |
|---|---|
| LVGL `refr` 16ms | `gpu_disp_set_refr_60hz()` |
| dirty + last flush present | 取消每脏区 SetCrtc |
| `drmModePageFlip` + vblank 等待 | 首帧仍 SetCrtc |
| `eglSwapInterval(1)` | 若符号存在 |
| 主循环 GPU：`invalidate` + sleep≤16 | 空闲也冲约 60 present/s |
| 屏上「帧率」 | `gpu_disp_fps()` 约 1s 窗 |

板验：

| 版本 | 帧率 |
|---|---|
| 0.4.1 | ~39（PageFlip + 主循环再 sleep） |
| **0.4.2** | **~61**（去掉双重等待） |

dumb：`FORCE_DUMB=1`（右上角 `— fps`）。

### 下一步（步骤 3，可选）

dumb vs GPU：CPU%、GPU%、帧率、MemAvailable、观感；FORCE_DUMB 注入。不挡 PHASE7 A。

---

## 结论

- **步骤 1 / 2 / 2b 关闭**（GPU + 约 60fps 板上 OK）。  
- PHASE7：**E 已前移完成**；接下 A–D（见 PHASE7.md）。
