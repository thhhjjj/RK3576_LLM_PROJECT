# Qwen3.5-2B → RK3576（`.rkllm` 导出）

把 Hugging Face 上的 **Qwen/Qwen3.5-2B** 转成板端可用的 **W4A16** 语言侧 `.rkllm`。

## 推荐：直接用社区预转换包

无需本目录脚本。下载后放到 `rkllm_autoload/models/` 即可（见上级 `README.md`）：

```text
https://huggingface.co/HanzoHuang/Qwen3.5-2B-RKLLM
  RK3576/Qwen3.5-2B_RK3576_w4a16_g128.rkllm
  RK3576/Qwen3.5-2B_vision_RK3576.rknn
```

当前 `llm_panel` 默认 `max_context_len=4096`，须与 `.rkllm` 导出时的上限一致（运行时不可大于导出值）。

## 自行导出（可选）

| 环境 | 说明 |
|---|---|
| **Linux x86_64** + **RKLLM Toolkit ≥ 1.3.0** | 可执行 `build` / `export_rkllm`（有 NVIDIA GPU 更稳；纯 CPU 极慢、吃内存） |
| macOS / 板端 aarch64 | 只适合拉 HF 权重；**不能**装官方 Toolkit 做正式导出 |

板端 `librkllmrt.so` 与 Toolkit **同大版本**（本仓库随附 1.3.x）。

```bash
cd rkllm_autoload

# 1) 拉上游权重（Mac / Linux 均可）
#    直连慢时可：export HF_ENDPOINT=https://hf-mirror.com
./export/fetch_qwen35_2b_hf.sh ./hf_src/Qwen3.5-2B

# 2) 在已安装 toolkit 的 Linux 上导出
python3 export/export_qwen35_2b_rk3576_8k.py \
  --model ./hf_src/Qwen3.5-2B \
  --out ./models/QWEN3.5_2B.rkllm \
  --dataset /path/to/rknn-llm/examples/rkllm_api_demo/export/data_quant.json \
  --device cpu \
  --max-context 4096 \
  --platform rk3576 \
  --quantized-dtype W4A16 \
  --quantized-algorithm grq
```

有 GPU 时把 `--device` 改为 `cuda`。需要更长窗口时增大 `--max-context`（如 `8192`），并同步改 panel 的 `LLM_PANEL_MAX_CONTEXT`；4GB 板子上长上下文容易 OOM。

脚本名里的 `8k` 仅表示可调到 8192；默认参数已按 **4096** 产品线书写。

## 本目录文件

| 文件 | 作用 |
|---|---|
| `fetch_qwen35_2b_hf.sh` | 下载 `Qwen/Qwen3.5-2B` |
| `export_qwen35_2b_rk3576_8k.py` | 调用 `rkllm.api` 量化并 `export_rkllm` |
