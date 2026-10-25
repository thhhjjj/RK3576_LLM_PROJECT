# PHASE7 已做记录

> 对照 [PHASE7.md](./PHASE7.md) / [project.md](../project.md) 第七阶段。

## 进度

| 块 | 状态 |
|---|---|
| E 60fps | **已过**（并入 PHASE6，0.4.2 ≈ 61fps） |
| A 调频 | **部分** — boost 生效；屏上 tok/s **~6.1**（未达 8～9） |
| B 长上下文 | **代码 0.6.0 → 2048**（须换 export≥2048 的 `.rkllm`） |
| C / D | 未做 |

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

## 结论

- E 关闭；A 升频可用、tok/s≈6.1 未达 8～9。  
- **B：代码已切 2048；缺 ≥2048 导出模型则 load 会挂。**
