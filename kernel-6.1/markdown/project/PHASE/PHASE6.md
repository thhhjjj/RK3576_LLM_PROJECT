# 第六阶段：做到哪一步

> 依据 [project.md](../project.md) 的 **LVGL → GPU 渲染**。**主线阶段**（与 3、7 并列优先于 BLE/frpc 添头）。本阶段结束时：UI 绘制不再只靠 CPU 填 dumb buffer；显示侧稳定双缓冲（或 GPU 合成等价）；空闲/对话时 CPU 占用下降、GPU 负载合理升高；失败可回退现有 DRM dumb 路径。

排期：**PHASE3 收口后可紧接本阶段**，不必等 4/5。回退 dumb 路径必须一直能编过。

已做记录见 [PHASE6_DID.md](./PHASE6_DID.md)。

## 完成线

同时满足下面几条才算完成：

1. **绘制后端**：LVGL 像素路径走 **GPU**（Rockchip Mali + GBM/EGL，或 SDK 已有 LVGL GPU / 2.5D 合成）；输出仍是 MIPI **720×1280**。  
2. **缓冲**：显示侧默认 **双缓冲**（约 2×framebuffer ≈ 7MB 量级可接受）；**无明显撕裂**。  
3. **不变式**：触摸 `evdev`；**先 `session_load` 再开 UI**；`tokq` / WS / 云端逻辑不因换显示后端改语义。  
4. **验收**：空闲与流式对话时 CPU↓、屏上 GPU 负载有合理↑；无花屏、无与 RKLLM 抢内存 OOM；`MemAvailable` / token/s 不劣于 CPU 方案（差几 MB 不算回归）。  
5. **回退**：宏或运行时可选回 **DRM dumb + CPU SW**；产品在 GPU 失败时仍可交付。

## 现状锚点（开工前已摸清）

当前 `llm_panel` → `lv_drivers` `drm_disp_drv_init`：

| 项 | 实际 |
|---|---|
| 显示 | DRM Atomic + plane，**dumb buffer** mmap |
| 缓冲 | 源码已分配 **`drm_bufs[2]`**，`drm_flush` 里翻页（`cur_bufs`）——**显示侧已是双 dumb**，不是「只有一块 FB」 |
| 绘制 | **LVGL 软件渲染**往 `map` 里填像素（CPU）；**尚未**走 Mali/EGL |
| 瓶颈口径 | 对话卡顿主因仍是 NPU；PHASE6 目标是减 UI CPU、减撕裂感、为 60fps/美术打底 |

> 订正：[project.md](../project.md) 写「多为单缓冲」——对 **LVGL 绘制** 而言是 CPU 单路径；对 **DRM FB** 而言 upstream `drm.c` 已是双 dumb。本阶段要把「绘制」迁到 GPU，而不是从零发明双缓冲。

## 四块各自停在哪

| 块 | 做到这个程度 | 停在这里 |
|---|---|---|
| 评估 | 板上 Mali 节点 / `libmali` / EGL / GBM 有无；选定后端 | 不一次上 Wayland 全家桶 |
| 集成 | `llm_panel` 可切换 GPU 与 dumb | 不改业务 UI 布局语义 |
| 性能 | DID 记 CPU%/GPU%/撕裂观感 | 不追求游戏级帧率 |
| 回退 | 一键或默认 fallback dumb | 不删 dumb 代码 |

## 细分步骤

### 1. 评估（本阶段第一步）

1. 板端：`/dev/dri/*`、`/sys/class/devfreq/*mali*` 或 `*gpu*`、`libEGL`/`libgbm`/`libmali` 是否在 rootfs。  
2. SDK/Buildroot：有无 Rockchip Mali 用户态包、头文件、与 kernel 匹配的 blob。  
3. 二选一（或记下「本树暂无 blob → 只做双缓冲打磨 + 文档」）：  
   - **A**：GBM + EGL + GLES 纹理 → LVGL `flush` 或 GPU 驱动  
   - **B**：SDK 现成 LVGL GPU / RGA / 2.5D 路径  
4. 结论写入 PHASE6_DID。

### 2. 落地 GPU 路径

5. 新增显示后端（或补丁 `lv_drivers`），与现有 `drm_disp_drv_init` 并存。  
6. CMake / `llm_panel.mk` 增加可选依赖；无库时仍链 dumb。  
7. 主循环仍 `lv_timer_handler`；不在 RKLLM 回调里碰 GL。

### 3. 验收与回退

8. 对比表：dumb vs GPU 的 CPU%、GPU%、MemAvailable、主观撕裂。  
9. 故障注入：卸 lib / 强制 fallback，确认 UI 仍起。  
10. 收口 DID；**不**把本阶段绑成 PHASE3/4/5 的硬门。

## 明确不做

- 为 GPU 重写对话业务逻辑  
- 强上 Wayland/Weston 桌面  
- 未评估就删 dumb 回退  
- 与 PHASE7 调频耦合（可衔接，但不挡本阶段出口）

## 当前状态

**主路径已过**：mali/GPU 显示 + **约 61fps**（0.4.2）；dumb 可回退。可选对比表见 [PHASE6_DID.md](./PHASE6_DID.md)。  
主线后续：[PHASE7.md](./PHASE7.md) **A 调频**（E 已前移完成）。
