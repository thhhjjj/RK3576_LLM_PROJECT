# PHASE2 已做记录

> 对照 [PHASE2.md](./PHASE2.md)。只记已经落地的改动；未做步骤标「未做」。第一阶段基线见 [PHASE1_DID.md](./PHASE1_DID.md)。  
> 刷机 / overlay 版本陷阱见 [version_must_care.md](./version_must_care.md)。

## 进度总览

| PHASE2 步骤块 | 状态 |
|---|---|
| 1. 运行时进 rootfs | **已过**。`librkllmrt` 1.3.1 + `libgomp.so.1` 经 `prepare.sh` 进镜像；板上 `dlopen` 成功 |
| 2. 冒烟 / 试跑模型 | **已过（文本）**。Qwen3.5-2B VLM 语言侧 **W4A16_G128** @1024；点「试问」有中文短答（2026-10-18） |
| 3. 改 `llm_panel` 进程结构 | **已过**。先 `session_load` 再 LVGL；`tokq`；worker 线程；预置「试问」 |
| 4. Qwen3.5-2B 文本多轮 + 压缩 | **已过（基本）**。试问 + 手动压缩 + prefill 后上下文恢复（`0.0.6`，2026-10-18）；多轮打满 70% / 压缩后 Mem 回升 **待记全**。**压缩仍同步 → LVGL 假死，体验未过（见下「K」）** |
| 5. 内存低水位与异常路径 | **代码已做**（`MemAvailable` &lt; 200MB → 告警 + 压缩，滞回解除）。极端低水位板测、低水位同步压缩卡屏 → 移交 [project.md](../project.md) **第七阶段** |
| 6. 视觉通路 / 1.7B 对照 / 2048 | **不做已过**（`vision.rknn` 可后装；不挡文本验收） |
| 7. 再裁 rootfs / 关无用服务 | **未做**（PHASE2 后续，不挡完成线；见下「L」与 [PHASE2.md](./PHASE2.md)「后续工作」） |
| 8. 收口 | **未做**（模型是否烤进镜像、文档收口、对照表） |

**入口条件（PHASE1）**：屏 / 触摸 / Seekwave / 裁剪 Buildroot / `llm_ctrl` / `llm_panel`（中文）已过。

**继承内存基线（LVGL+中文，无模型，2026-10-17）**：MemAvailable ≈ **3780 MB**；面板约 `118 / 3898 MB`。

**试跑加载后（2026-10-18，屏上）**：内存约 **`1705 / 3898 MB`**（已用/总计）；上下文曾到 `549 / 1024`（含 thinking 浪费，已用 `/no_think` + 截断缓解）。

日常差分仍以 **`Desktop/makediff`**（或云端 `RK3576_LLM_PROJECT`）为提交源。改 panel 日常可用 **`adb push llm_panel`** 快验，整包 rootfs 留给依赖/收口。

---

## 版本与路径

| 项 | 值 |
|---|---|
| PC / 板端 RKLLM | **1.3.1**（`rkllm_autoload/librkllmrt.so`；日志 `rkllm-runtime version: 1.3.1`） |
| 模型 dtype | **W4A16_G128**（日志 `model_dtype: W4A16_G128`） |
| 语言模型板上路径 | `/usr/share/rkllm/QWEN3.5_2B.rkllm`（桌面同名；可另有 `default.rkllm` → 软链） |
| 视觉模型 | `/usr/share/rkllm/vision.rknn`（可选；文本试跑可不装） |
| OpenMP 依赖 | `/usr/lib/libgomp.so.1`（`librkllmrt` NEEDED；Debian arm64 抽入 `rkllm_autoload/`） |
| HF 来源 | `HanzoHuang/Qwen3.5-2B-RKLLM` / `RK3576/` |
| 主进程 | `/usr/bin/llm_panel`；`S90llm_panel` 自启 |
| UI 版本标 | 右下角白色 **`vtest：0.0.6`**（thinking 过滤 + 低水位 + 压缩后 prefill） |
| 暂存 | `makediff/rkllm_autoload/` → SDK `rkllm_autoload/` → `prepare.sh` |

环境变量：`LLM_PANEL_LIB`、`LLM_PANEL_MODEL`。

---

## `MemAvailable` / 面板内存记账

| 场景 | 数值 | 日期 |
|---|---|---|
| PHASE1 基线（LVGL+中文，无模型） | ≈3780 MB Available；面板 `118 / 3898 MB` | 2026-10-17 |
| Qwen3.5-2B 语言侧加载后（UI 已起） | 面板约 **`1705 / 3898 MB`**；`/proc` **MemAvailable ≈ 2191 MB**（`2243484 kB`） | 2026-10-18 |
| 试问后上下文 | 曾 **`549 / 1024`**（thinking 未截断时）；`0.0.4` 起截断 | 2026-10-18 |
| 多轮接近水位 / 压缩后 | | |
| 挂上 vision.rknn 后 | | |
| 再裁服务前（加载模型后对照） | | |
| 再裁服务后（无模型基线） | | |
| 再裁服务后（加载模型后） | | |

---

## 改动明细（代码 / overlay）

### 1. `rkllm_autoload/` + `prepare.sh`

| 项 | 内容 |
|---|---|
| Runtime | `librkllmrt.so` 1.3.1 → `/usr/lib/librkllmrt.so` |
| OpenMP | `libgomp.so.1` → `/usr/lib/libgomp.so.1`（及 `libgomp.so` 软链） |
| 模型名 | 安装为 **`QWEN3.5_2B.rkllm`**（兼容旧 `models/default.rkllm` 输入） |
| 视觉 | 可选 `vision.rknn` |
| 工具 | `fetch_qwen35_2b_vlm_rk3576.sh` 下载到 `models/QWEN3.5_2B.rkllm` |

### 2. `llm_panel`（PHASE2 结构）

| 文件 | 作用 |
|---|---|
| `tokq.c` / `tokq.h` | 回调 → UI 有界队列 |
| `rkllm_session.c` / `.h` | `dlopen`、worker、`session_load/ask/abort/compress`；默认模型路径；**`/no_think`**；截断 Thinking Process |
| `vendor/rkllm.h` | 官方 1.3.1 API 头 |
| `main.c` | **先 `session_load` 再 `hal_init` / `ui_create`** |
| `ui.c` | 试问 / 压缩 / 上下文 / token/s；右下角 `vtest：x.y.z` |

输入：预置「试问」（无拼音）。云端开关仍 UI-only。

### 3. 文档

| 文件 | 内容 |
|---|---|
| [version_must_care.md](./version_must_care.md) | makediff → SDK → 编 → 刷 链路；`bash git_push`；强制 `llm_panel-rebuild` |
| `rkllm_autoload/README.md` | 路径、依赖、验收顺序 |

---

## 分步记录

### 1. 运行时进 rootfs — **已过**

- `release-v1.3.1` so + 头文件进 makediff。  
- 板上曾缺 **`libgomp.so.1`** → `dlopen` 失败；已抽 Debian arm64 包放入 `rkllm_autoload/`，`prepare.sh` 固化。  
- 刷机后确认：`librkllmrt` + `libgomp` 在 `/usr/lib/`。

### 2. 冒烟模型 — **已过（文本）**

- 模型文件约 2.2G；板上路径 `QWEN3.5_2B.rkllm`（与桌面对齐）。  
- 日志：`model_dtype: W4A16_G128`，`platform: RK3576`，`model=yes`。  
- 「试问」预置句：用一句话介绍 RK3576 上的 RKNPU → 有中文短答。  
- **未**把 2.2G 烤进日常迭代镜像（收口再定）；可用 adb 推或偶发打进 rootfs。

### 3. 进程结构 — **已过**

硬顺序：load → UI。回调只 `tokq_push`；LVGL 定时器 `drain_tokq`。

### 4. 多轮 + 压缩 — **部分**

试问通；压缩 API 已接。系统多轮水位与压缩后 `MemAvailable` 回升 **未记完整表**。

### 5. 内存与异常路径 — **代码已做**

- 压缩按钮 → `session_compress`；上下文 **70%** 自动压缩。
- **`MemAvailable` &lt; 200MB**：对话区告警并触发一次压缩；&gt;250MB 解除闩锁（`main.c`，`vtest：0.0.5`）。
- 缺模型：UI「未加载」，进程不崩。板上打到 &lt;200MB 极端低水位、低水位路径不卡屏 → **第七阶段**验收（本阶段只保证代码路径在）。

### 6. 可选压测 — **未做**

### 7. 收口 — **未做**

是否烤 `QWEN3.5_2B.rkllm` 进镜像；冷启动清单；完成线七条核对。

### 8. 再裁 rootfs / 关无用服务 — **未做**（后续）

目的：在 PHASE1 已裁镜像之上，再关 Docker / 桌面 / 多余 daemon，争取 **几十～约 100～200 MB** `MemAvailable`，给更长上下文留余量。  
不挡完成线；压缩异步（**K**）优先。口径与步骤见 [PHASE2.md](./PHASE2.md)「后续工作」与步骤 27–30。

---

## 问题怎么解决的（按时间线）

### A. 刷了很大 rootfs，板上仍是旧 `llm_panel`

源码新、**Buildroot 未重编 panel**（构建目录甚至不存在）。`./build.sh rootfs` 不保证重编已安装包。  
→ `make … llm_panel-dirclean llm_panel-rebuild`，再看 `target` 的 `strings`。

### B. `makediff` 有改动，SDK 没有

`./makediff/git_push_must_change.sh` → **Permission denied**（SMB 无执行位）。脚本没跑 = 没落地。  
→ 永远用：`bash makediff/git_push_must_change.sh makediff`，再 `grep` **SDK** 路径。

### C. 版本标加在标题后看不见

与右侧「模型: …」叠字。  
→ 右下角独立标签 `vtest：x.y.z`。

### D. `dlopen` 失败：`libgomp.so.1`

Buildroot output 无 libgomp；`librkllmrt` 依赖它。  
→ 放入 `rkllm_autoload/libgomp.so.1` + `prepare.sh`；adb 可先推验证。

### E. 模型文件名

桌面 `QWEN3.5_2B.rkllm`，旧默认 `default.rkllm`。新 panel 找前者；镜像里若只有后者 → `mv` + 软链。`prepare.sh` 现统一装成 `QWEN3.5_2B.rkllm`。

### F. 英文 Thinking Process 占 token、拖慢

`enable_thinking=false` 在 `set_chat_template` 后不可靠。  
→ 每轮追加 `/no_think`；流式检测 `Thinking Process`：先想则跳过等中文，先答后想则截断并 abort。

### G. `0.0.3` 试问无答复

过滤器过早 abort（思维在前时答案未出）。  
→ `0.0.4` 区分 leading / trailing think。

### H. 快验不必整包

改 UI/会话：VM 编出 `llm_panel` → 拷桌面 → `adb push /usr/bin/llm_panel` → 看右下角版本号。整包留给 so/gomp/镜像依赖。

### I. 内存低水位（步骤 18）

`metrics_timer_cb` 读 `MemAvailable`；&lt;200MB 告警 + `session_compress`，滞回 250MB。  
极端低水位板测、以及低水位触发仍同步压缩会卡屏 → 不挡 PHASE2 代码勾选，收进 [project.md](../project.md) **第七阶段**。

### J. 压缩后上下文归零

`session_compress` 只 `destroy`+`create`，**未把保留历史 prefill 回新句柄**，KV 空 → 上下文显示没了。  
→ 重建后 `prefill_retained_hist()`（`vtest：0.0.6`）。

### K. 压缩时整屏 LVGL 卡住（体验，2026-10-19）

**现象**：点「压缩」或低水位触发后，对话区能先刷出「压缩中…」，随后整屏假死（指标停、触摸无响应），直到压缩结束。

**根因**：`on_ui_compress` / `metrics_timer_cb` 在 LVGL 线程同步调 `session_compress()`（destroy → 整模 `create_handle` → `prefill_retained_hist`），堵住 `lv_timer_handler`。worker 路径上的压缩不堵 UI；**按钮与低水位这两条会堵**。

**应对（未做）**：与 `session_ask` 对齐——主线程只投递压缩任务；worker 执行；`tokq` 推 `[压缩完成]` / 失败；压缩中拒 ask、防重复点。改完后验收：压缩全程六项指标仍刷新。

### L. 再裁 rootfs / 关无用服务（后续，未做）

**动机**：冲更长上下文时，系统常驻仍可能白占几十～一两百 MB Available；值得在模型侧手段之外做第二轮用户态裁剪。

**范围**：盘点并禁用确认无用的自启（Docker、桌面会话、未用中间件等）；可只关自启、保留后续阶段软件包。  
**红线**：不砍 NPU/DRM/CMA、Seekwave、指标用的 proc/sysfs（同 [project.md](../project.md) 裁剪红线）。

**验收**：裁前/裁后各记无模型基线与加载模型后 `MemAvailable`（填上表空行）；DID 写明关掉的单元名与差值。无明显回升则停止空砍。

细节口诀见 [version_must_care.md](./version_must_care.md)。

---

## 板上验收口令（文本试跑）

```bash
adb shell "strings /usr/bin/llm_panel | grep -E '0\.0\.5|no_think|低水位'"
adb shell 'ls -lh /usr/lib/librkllmrt.so /usr/lib/libgomp.so.1 /usr/share/rkllm/QWEN3.5_2B.rkllm'
adb shell 'grep -E "model=|loaded|skip leading|cut trailing" /tmp/llm_panel.log | tail'
```

屏上：右下角 **`vtest：0.0.5`**；「模型: 已加载」；试问 → 中文短答，无大段 Thinking Process。点「压缩」应出现 `[压缩完成]` 类状态。

---

## 驱动 / 用户态结论（截至 2026-10-18）

- **已过（继承 PHASE1）**：MIPI、FTS、Seekwave、裁剪 Buildroot、`llm_ctrl`、`llm_panel` 显示+触摸+中文。  
- **本阶段已过**：RKLLM 1.3.1 + libgomp；先 load 再 UI；Qwen3.5-2B W4A16_G128 文本试问；thinking 抑制；内存低水位压缩（代码）；压缩后 prefill（`0.0.6`）；版本标签快验。  
- **已知体验问题**：压缩同步跑在 LVGL 线程 → 整屏卡死（见 **K**）；低水位同路径。异步化 + 极端低水位实压 → **第七阶段**（PHASE2 代码路径已在）。  
- **下一步**：多轮/手动压缩后记 `MemAvailable`；收口是否烤模型；其后做 **再裁 rootfs / 关无用服务（L，步骤 27–30）**；卡屏/低水位板测见第七阶段。  
- **不做**：WebSocket、GATT、云端 SSE、frpc、拼音输入法（本阶段仍不开；裁服务时勿误删后续要用的包，可先只关自启）。
