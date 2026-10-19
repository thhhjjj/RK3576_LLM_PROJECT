# llm_panel 带读说明（含行号）

> 板上主 UI：`/usr/bin/llm_panel`，720×1280 DRM + LVGL。  
> **行号以当前仓库为准**；若本地改过文件，以编辑器里 `Ctrl+G` / 大纲为准。  
> IDE：`Ctrl+P` 输文件名 → `Ctrl+G` 输行号。下文写作 `` `文件:行号` ``。

---

## 0. 一分钟概念

```text
main 线程：LVGL + 定时器 drain tokq + 指标
worker 线程：rkllm_run，回调只 tokq_push
tokq：两边之间的有界队列（禁止回调画屏）
```

硬顺序：`session_load` → 再 `hal_init` / `ui_create`（见 `main.c:158-171`）。

---

## 1. 跳转总表（先收藏）

### `main.c`（总控，198 行）

| 行号 | 符号 | 干什么 |
|---:|---|---|
| 24 | `MEM_LOW_WATER_MB` | 内存低水位 200MB |
| 26 | `on_sig` | SIGINT/SIGTERM → 退出循环 |
| 32 | `on_ui_compress` | 「压缩」按钮 → `session_compress` |
| 39 | `on_ui_ask` | 「试问」→ 写对话框 + `session_ask` |
| 54 | `drain_tokq` | 弹出 tokq 事件刷 UI |
| 61–84 | `switch (ev.kind)` | TOKEN/FINISH/ERROR/STATUS 分支 |
| 89 | `metrics_timer_cb` | ~100ms：drain + 指标 + 低水位压缩 |
| 103–121 | 低水位逻辑 | `<200MB` 压缩；`>250MB` 解除滞回 |
| 125 | `hal_init` | `lv_init` + DRM + evdev |
| 145 | **`main`** | 入口；先 load 再 UI |
| 155–156 | | `session_init` / `metrics_init` |
| 161 | | `session_load(NULL)` |
| 166–171 | | `hal_init` → `font_init` → `ui_set_actions` → `ui_create` |
| 181 | | `lv_timer_create(..., 100, ...)` |
| 187–193 | | `lv_timer_handler` 主循环 |
| 195 | | `session_fini` |

### `ui.c` / `ui.h`

| 行号 | 符号 | 干什么 |
|---:|---|---|
| `ui.h:7-19` | 对外 API | create / metrics / dialog / actions |
| `ui.c:47` | `on_compress` | LVGL 事件 → `act_compress` |
| `ui.c:54` | `on_ask` | LVGL 事件 → `act_ask` |
| `ui.c:61` | `on_mode` | 本地/云端开关（现只改字「云端(未接)」） |
| `ui.c:85` | `ui_set_actions` | main 注入两个回调 |
| `ui.c:91` | **`ui_create`** | 整屏布局 |
| `ui.c:96-99` | 标题 | 「LLM 终端」 |
| `ui.c:106-116` | 指标行 | 内存…token/s |
| `ui.c:118-126` | `ta_dialog` | 对话区 |
| `ui.c:128-148` | 压缩 / 试问按钮 | y≈980 |
| `ui.c:150-157` | 模式开关 | |
| `ui.c:165-171` | `vtest：0.0.6` | 右下角版本标 |
| `ui.c:174` | `ui_update_metrics` | 刷新六项指标 |
| `ui.c:231` | `ui_append_dialog` | 追加对话文本 |
| `ui.c:237` | `ui_set_ctx_tps` | 上下文 + token/s |
| `ui.c:255` | `ui_set_model_status` | 「模型: 已加载」 |

### `tokq.c` / `tokq.h`

| 行号 | 符号 | 干什么 |
|---:|---|---|
| `tokq.h:10-15` | `tokq_kind` | TOKEN / FINISH / ERROR / STATUS |
| `tokq.h:17-20` | `tokq_evt` | kind + `text[256]` |
| `tokq.c:7` | `TOKQ_CAP` | 队列深度 128 |
| `tokq.c:13` | `tokq_init` | |
| `tokq.c:26` | **`tokq_push`** | 回调侧；满则 -1 丢弃、不阻塞 |
| `tokq.c:47` | **`tokq_pop`** | UI 侧 |

### `rkllm_session.h`（对外契约）

| 行号 | 符号 | 干什么 |
|---:|---|---|
| 11–12 | 默认 so / 模型路径 | `librkllmrt.so` / `QWEN3.5_2B.rkllm` |
| 14 | 视觉路径 | 预留，本阶段不用 |
| 15 | `LLM_PANEL_MAX_CONTEXT` | **1024** |
| 16 | `COMPRESS_WATER_PCT` | **70%** |
| 18–24 | `session_stats` | loaded / ctx / tps |
| 26–48 | API 声明 | init/load/ask/abort/compress/… |

### `rkllm_session.c`（推理核心，703 行）

| 行号 | 符号 | 干什么 |
|---:|---|---|
| 20–22 | `HIST_MAX` / `MSG_MAX` / `PROMPT_MAX` | 历史条数 / 单条长 / 回复累加缓冲 |
| 24–26 | `SYSTEM_PROMPT` | 系统人设（软约束，不等于 no_think） |
| 28 | `SMOKE_Q` | 试问固定句 |
| 33–36 | `hist_msg` | 一条历史：role + text |
| 63–64 | `hist[]` / `hist_n` | 历史数组 |
| 66–72 | worker 同步 | mutex/cond/`pending_user` |
| 79–80 | `reply_acc` | 本轮助手全文累加（截 thinking） |
| 91 | `hist_reset_keep_system` | 只留 system |
| 100 | `hist_push` | 追加一轮；满则挤掉最旧 |
| 117 | `refresh_ctx` | `rkllm_get_kv_cache_size` → `last_ctx` |
| 129 | `find_thinking_mark` | 找 Thinking / `<think>` 等 |
| 151 | `first_cjk_utf8` | 跳过英文 thinking 找中文答案 |
| 164 | `trim_thinking_tail` | FINISH 前再砍尾巴 |
| 180 | **`result_cb`** | RKLLM 回调（只 push tokq） |
| 184–194 | | `RUN_ERROR` |
| 205–210 | | `prefilling`：abort，不刷 UI |
| 218–235 | | 先 think 后答：丢 think，等 CJK |
| 246–289 | | 答后跟 think：截断并 abort |
| 292 | | 正常 TOKEN → `tokq_push` |
| 296–327 | | `RUN_FINISH`：算 tps、`hist_push('a')`、`TOKQ_FINISH` |
| 332 | `resolve_syms` | `dlsym` 各 API |
| 354 | `open_lib` | `dlopen` so |
| 369 | `destroy_handle` | destroy |
| 379 | **`create_handle`** | `rkllm_init` + chat_template |
| 408–413 | | `set_chat_template(SYSTEM_PROMPT…)` |
| 417 | **`worker_main`** | 等 job → `rkllm_run` |
| 455–474 | | **`/no_think`** + `enable_thinking=false` |
| 479–484 | | 上下文 ≥70% → 自动压缩 |
| 493 | `session_init` | hist + tokq |
| 503 | `session_fini` | 停 worker、destroy、dlclose |
| 521 | **`session_load`** | 路径解析 → create → 起 worker |
| 525–528 | | `LLM_PANEL_MODEL` 环境变量 / 默认路径 |
| 558 | `session_loaded` | |
| 563 | **`session_ask`** | 排队；忙返回 -1 |
| 589 | `session_abort` | |
| 596 | `prefill_retained_hist` | 压缩后把历史灌回 KV |
| 648 | **`session_compress`** | 裁 hist → recreate → prefill |
| 660–670 | | 保留 system + 最近约 4 轮 |
| 688 | `session_get_stats` | |
| 699 | `session_smoke_question` | 返回 `SMOKE_Q` |

### `metrics.c` / `evdev.c` / `font.c`

| 行号 | 符号 | 干什么 |
|---:|---|---|
| `metrics.c:13-18` | 路径宏 | llm_ctrl 温度、`/proc/rknpu/load`、meminfo… |
| `metrics.c:118` | `metrics_init` | 发现 GPU load 路径 |
| `metrics.c:130` | `sample_mem` | |
| `metrics.c:154` | `sample_cpu` | |
| `metrics.c:198` | `sample_npu_load` | |
| `metrics.c:229` | `sample_npu_temp` | |
| `metrics.c:242` | `sample_gpu` | |
| `metrics.c:268` | `sample_net` | wlan0 |
| `metrics.c:320` | **`metrics_sample`** | 汇总 |
| `metrics_main.c:17` | `main` | 无 LVGL 串口冒烟 |
| `evdev.h:17-22` | 默认分辨率 | 720×1280 |
| `evdev.c:41` | `evdev_get_tp_event` | 找触摸节点 |
| `evdev.c:133` | `evdev_init` | |
| `evdev.c:143` | `evdev_read` | LVGL indev 回调 |
| `font.c:19` | `font_init` | 加载思源 OTF |

### `vendor/rkllm.h`（常用 API）

| 行号 | API |
|---:|---|
| 360 | `rkllm_init` |
| 390 | `rkllm_destroy` |
| 402 | `rkllm_run` |
| 421 | `rkllm_abort` |
| 428 | `rkllm_is_running` |
| 456 | `rkllm_get_kv_cache_size` |
| 472 | `rkllm_set_chat_template` |

---

## 2. 启动：按行跟 `main`

1. `main.c:155-156` — `session_init`（`rkllm_session.c:493`：重置 hist、`tokq_init`）  
2. `main.c:161` — **`session_load(NULL)`** → `rkllm_session.c:521`  
   - `525-528` 解析路径（环境变量或默认）  
   - `533` `open_lib`（`354`）`dlopen`  
   - `539` `create_handle`（`379`）：`rkllm_init` + `412-413` 设 system  
   - `545-552` 创建 `worker_main`（`417`）  
3. `main.c:166` — `hal_init`（`125`）：LVGL + DRM + `evdev_init`（`evdev.c:133`）  
4. `main.c:170-171` — 绑定按钮回调并 `ui_create`（`ui.c:91`）  
5. `main.c:181` — 100ms 定时器 → `metrics_timer_cb`（`89`）  
6. `main.c:187-193` — 主循环只跑 `lv_timer_handler`

---

## 3. 点「试问」：按行跟完一条链

```text
ui.c:54 on_ask
  → main.c:39 on_ui_ask
       41  session_smoke_question → rkllm_session.c:699 → SMOKE_Q (28)
       43  未加载则提示并 return
       47-49 对话框写「你/助手」
       50  session_ask(q) → rkllm_session.c:563
            581-584  pending_user + signal worker
  → worker_main rkllm_session.c:417
       443  hist_push('u')
       462-463  prompt = user + "\n/no_think"   ← 真·关 thinking
       466  enable_thinking = false
       474  rkllm_run → 回调 result_cb (180)
            292  tokq_push(TOKEN)     tokq.c:26
            325  tokq_push(FINISH)
  → 定时器 main.c:96 drain_tokq (54)
       61-62 TOKEN → ui_append_dialog (ui.c:231)
       64-68 FINISH → 刷新 ctx/tps
```

**系统提示 vs `/no_think`：**

- 人设：`rkllm_session.c:24-26` + `412-413` `set_chat_template`  
- 关 thinking：`462-466`（注释 `455-457` 写明 flag 不够，靠 `/no_think`）  
- 漏网截断：`result_cb` `218-289`、`trim_thinking_tail` `164`

---

## 4. 压缩：按行跟

| 入口 | 行号 |
|---|---|
| 按钮 | `ui.c:47` → `main.c:32` → `session_compress` `648` |
| 低水位 | `main.c:103-116`（`< MEM_LOW_WATER_MB`） |
| 自动（聊完） | `worker_main` `479-484`（≥70% context） |

`session_compress`（`648`）步骤：

1. `655-656` 推理中拒绝  
2. `660-670` 历史裁到 system + 近几轮  
3. `673-678` destroy + `create_handle`（KV 空）  
4. `681` **`prefill_retained_hist`（`596`）** 把历史灌回  
5. `684` `tokq_push(STATUS, "[压缩完成]")`

prefill 时 `result_cb` `205-209`：直接 `abort`，不往 UI 吐字。

---

## 5. 缓冲宏（`rkllm_session.c:20-22`）

| 宏 | 行 | 用途 |
|---|---:|---|
| `HIST_MAX` 32 | 20 | `hist[32]` 最多 32 条消息 |
| `MSG_MAX` 4096 | 21 | 单条 `text[]`、`pending_user` |
| `PROMPT_MAX` 256KiB | 22 | `reply_acc`、prefill 拼包 |

与 **`LLM_PANEL_MAX_CONTEXT 1024`**（`rkllm_session.h:15`）无关：后者是 RKLLM token 窗口。

---

## 6. UI 布局对照（`ui_create` 内）

| 控件 | 约略行 | 坐标线索 |
|---|---:|---|
| 标题 / 模型 | 96–104 | y≈12–16 |
| 六项系统指标 | 106–111 | y 48–168 |
| 上下文 / tps | 113–116 | y 200–224 |
| 对话区 | 118–126 | y 260，高 680 |
| 压缩 / 试问 | 128–148 | y 980 |
| 模式开关 | 150–157 | x 400+ |
| vtest 版本 | 165–171 | 右下角 |

---

## 7. 建议跟读顺序（带行号）

1. `main.c:145` → `161` → `166` → `181` → `187`  
2. `ui.c:91` 扫一遍布局；`85` `ui_set_actions`  
3. `tokq.h:10-29` + `tokq.c:26/47`  
4. `rkllm_session.h` 全文（短）  
5. `session_ask` `563` → `worker_main` `417` → `result_cb` `180`  
6. `session_compress` `648` + `prefill_retained_hist` `596`  
7. `metrics_timer_cb` `main.c:89` + `metrics.c:320`

读完应能回答：先 load 再 UI 在哪几行；token 如何进屏；为何回调不调 `ui_*`；`/no_think` 在哪一行拼上。

---

## 8. 相关文档

| 文档 | 内容 |
|---|---|
| `../project.md` | 整机方案 / 模型选型 |
| `../PHASE/PHASE2.md` | 本应用完成线 |
| `../PHASE/PHASE2_DID.md` | 板上实测内存与版本 |
| `../PHASE/PHASE3.md` | 下一步：扇出（扩展 tokq）→ WebSocket |
| `../PHASE/version_must_care.md` | 改 panel 刷机版本陷阱 |

PHASE3：在 **`tokq_push` 之后** 加多订阅（UI+WS），不要在 `result_cb`（`180`）里写 socket。
