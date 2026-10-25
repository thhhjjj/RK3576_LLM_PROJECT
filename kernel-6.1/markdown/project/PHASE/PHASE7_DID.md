# PHASE7 已做记录

> 对照 [PHASE7.md](./PHASE7.md) / [project.md](../project.md) 第七阶段。

## 进度

| 块 | 状态 |
|---|---|
| E 60fps | **已过**（并入 PHASE6，0.4.2 ≈ 61fps） |
| A 调频 | **部分** — boost 生效；屏上 tok/s **~6.1**（未达 8～9） |
| B 长上下文 | **代码 0.6.0 → 2048**（须换 export≥2048 的 `.rkllm`） |
| C 摘要+异步压缩 | **代码 0.7.0+**（随 0.8.0） |
| D 体验清单 | **代码 0.8.0**；&lt;200MB 实压待做 |

---

## A. NPU 调频

### 板探（2026-10-24）

```text
/sys/class/devfreq/27700000.npu
available_frequencies: 300M … 950M
governor: rknpu_ondemand（空闲 cur≈300M）
available_governors: … userspace performance …
echo performance → cur=950000000
恢复 rknpu_ondemand OK
```

### 落地

| 文件 | 内容 |
|---|---|
| `npu_dvfs.c` / `.h` | 探测节点；`boost`→`performance`；`restore`→原 governor；可嵌套 |
| `rkllm_session.c` | **worker 里** `rkllm_run` 前后 boost/restore；compress prefill 同样；**不在回调里改频** |
| `ui.c` | `vtest：0.5.0` |

关闭：`LLM_PANEL_NPU_BOOST=0`

### 板验

```bash
# 重编推 0.5.0 后
adb shell 'grep npu_dvfs /tmp/llm_panel.log'
# 试问一次，期望：
#   npu_dvfs: … gov=rknpu_ondemand
#   npu_dvfs: boost … → performance cur=950000000
#   npu_dvfs: restore → rknpu_ondemand …
# 屏上 token/s：短问目标约 8～9（基线约 4.9）
```

基线/升频对比（屏上既有口径，含 prefill 墙钟）：

| 条件 | token/s | 备注 |
|---|---|---|
| 文档旧基线 | ~4.9 | project.md |
| 0.5.0 初测 | ~4.5 | boost 日志已有 |
| 0.5.0 再测 | ~5.9 | `performance@950M` |
| **0.5.0 再测** | **~6.1** | 同口径，波动中 |

未达目标 **8～9**。约束：不关推理期 60fps、不改 tok/s 统计口径。

---

## B. 长上下文 → 2048（先）

| 项 | 内容 |
|---|---|
| 代码 | `LLM_PANEL_MAX_CONTEXT` **1024 → 2048**（`rkllm_session.h`），`vtest：0.6.0` |
| 硬约束 | `max_context_len` **写进** `.rkllm`；运行时只能 ≤ 导出值。现用社区包多半是 **@1024**，只改 panel **不够** |
| 板验 | 冷启动 load 成功；日志 `max_context_len=2048`；屏上上下文分母 **2048**；记加载后 MemAvailable |

加载失败（init error）→ 仍是 1024 模型，需重新 export / 换包后再刷模型（可 `adb push` 测，固化再 rootfs）。

```bash
# panel 0.6.0
adb shell "strings /usr/bin/llm_panel | grep 0.6.0"
adb shell 'grep max_context /tmp/llm_panel.log'
# 期望 UI：xxx / 2048
```

加载后 Available（板测填）：

| 档 | 加载后 MemAvailable | 试问 | 压缩 |
|---|---|---|---|
| 1024（旧） | ~见 PHASE2 | OK | OK |
| **2048** | | | |

---

## C. 摘要记忆 + 异步压缩（0.7.0）

| 项 | 实现 |
|---|---|
| 人设 | `hist[0]` / `SYSTEM_PROMPT` **只读**；`hist_push` 拒 `'s'`/`'m'` |
| 摘要条 | role `'m'`，文案 `摘要: …`；有则替换、无则插在 s 后 |
| 近轮 | `KEEP_RECENT=4` |
| 摘要推理 | 丢弃段拼 prompt → `rkllm_run`（`keep_history=0`，`max_new_tokens=256`）；token 不上屏 |
| 异步 | `session_compress()` 只 `have_compress` 排队；worker 跑 `compress_do_work`；UI/低水位不堵 LVGL |
| 拒问 | 压缩中 `session_ask` 返回 -1 |
| 状态 | tokq：`[压缩已排队]` → `[摘要压缩中…]` → `[压缩完成·已更新摘要]` |

板验：多轮顶水位或点「压缩」→ 指标/触摸仍刷；完成后人设仍在、对话可续。

---

## D. 体验清单（0.8.0）

对照 project.md 表：

| 项 | 状态 |
|---|---|
| 压缩全程异步（按钮/低水位/70%） | **已做** — 均 `have_compress` 排队 |
| 极端 &lt;200MB 板测 | **未实压** — 代码路径同低水位；口令见下 |
| 单次 prefill 上限 | **已做** — `PREFILL_CHUNK=2048` 分段 |
| prompt_cache / n_keep | **不做**（本阶段） |
| 生成中停干净 | **已做** — `gen_inflight`；忙时拒 ask；UI「停止」 |
| 流式/采样档/输入法 | **不做**（本阶段） |
| thinking 抑制 | **已有** `/no_think` |
| 超温策略 | **已做** — NPU≥90℃ abort + 红条；&lt;80℃ 清 |
| 错误态可读 | **沿用** tokq ERROR / 缺模型文案 |
| 本地/云端切换停旧路 | **已有** `on_ui_mode` |

### &lt;200MB 实压口令（板上）

```bash
# 造压（示例，慎用）：临时吃内存后看面板是否告警并异步压缩、帧率是否还在刷
adb shell 'grep -E "低水位|压缩|超温|停止" /tmp/llm_panel.log | tail'
```

过程与 Available 填回本表后勾「实压过」。

---

## 结论

- E 关闭；A ≈6.1 未达 8～9。  
- B：代码 2048，待模型。  
- C/D：**0.8.0 代码收口**；D 仅缺 &lt;200MB 实压记录。
