# rkllm_autoload — PHASE2 运行时 / 模型暂存

本树没有正式的 Buildroot `librkllm` 包。把板端文件丢进这里，编 rootfs 时由 `overlays/30-llm_ctrl/prepare.sh` 装进镜像。

## 当前试跑选型（已定）

**Qwen3.5-2B VLM（语言 `.rkllm` + 视觉 `.rknn` 成对，RK3576）**

| 角色 | 文件（板上） |
|---|---|
| 语言侧 | `/usr/share/rkllm/QWEN3.5_2B.rkllm` |
| 视觉侧 | `/usr/share/rkllm/vision.rknn` |
| Runtime | `/usr/lib/librkllmrt.so`（**1.3.1**） |

社区预转换（推荐直接下，免自己 export）：

```text
https://huggingface.co/HanzoHuang/Qwen3.5-2B-RKLLM
  RK3576/Qwen3.5-2B_RK3576_w4a16_g128.rkllm
  RK3576/Qwen3.5-2B_vision_RK3576.rknn
```

本机一键下载到本目录（需网络 / `huggingface-cli` 或 `hf`）：

```bash
cd makediff/rkllm_autoload
./fetch_qwen35_2b_vlm_rk3576.sh
```

或手动：

```bash
hf download HanzoHuang/Qwen3.5-2B-RKLLM \
  RK3576/Qwen3.5-2B_RK3576_w4a16_g128.rkllm \
  RK3576/Qwen3.5-2B_vision_RK3576.rknn \
  --local-dir /tmp/qwen35-2b-rkllm

cp /tmp/qwen35-2b-rkllm/RK3576/Qwen3.5-2B_RK3576_w4a16_g128.rkllm \
   models/QWEN3.5_2B.rkllm
cp /tmp/qwen35-2b-rkllm/RK3576/Qwen3.5-2B_vision_RK3576.rknn \
   models/vision.rknn
```

**硬规则**：`.rkllm` 与 `.rknn` 必须同是 **RK3576** 目录一对；不可和 3588 混用。  
Toolkit / `librkllmrt`：**≥ 1.3.1**（已放 `librkllmrt.so`）。

## 放什么

| 本地路径（makediff） | 板上路径 |
|---|---|
| `rkllm_autoload/librkllmrt.so` | `/usr/lib/librkllmrt.so` |
| `rkllm_autoload/libomp.so`（若缺） | `/usr/lib/libomp.so` |
| `rkllm_autoload/libgomp.so.1`（**要**） | `/usr/lib/libgomp.so.1`（`librkllmrt` 依赖） |
| `rkllm_autoload/models/QWEN3.5_2B.rkllm` | `/usr/share/rkllm/QWEN3.5_2B.rkllm` |
| `rkllm_autoload/models/vision.rknn` | `/usr/share/rkllm/vision.rknn` |

头文件：`llm_panel/vendor/rkllm.h`（1.3.1；程序 `dlopen`）。

## 验收顺序（建议）

1. **只 load 语言 `.rkllm`**，点「试问」走文本流式（当前 `llm_panel` 先做这一步）。
2. 记加载后 / 多轮后 `MemAvailable`（4GB + LVGL + VLM 语言侧会紧一档）。
3. 视觉通路（图 → `RKLLM_INPUT_MULTIMODAL`）另开一轮；`vision.rknn` 先装进镜像备用。

## 板上检查

```bash
ls -lh /usr/lib/librkllmrt.so \
      /usr/share/rkllm/QWEN3.5_2B.rkllm \
      /usr/share/rkllm/vision.rknn
# 起 llm_panel：应先 load 再开屏；点「试问」
```

`.rkllm` / `.rknn` 默认不进 git（体积大）。`librkllmrt.so` 可提交。
