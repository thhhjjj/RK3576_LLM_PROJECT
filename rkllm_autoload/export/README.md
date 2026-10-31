# Qwen3.5-2B → RK3576 导出说明

**当前产品线**：用社区现成包（`max_context_limit: 4096`），panel 已对齐 `LLM_PANEL_MAX_CONTEXT 4096`。  
HF 原权重 / `Desktop/model` 暂存已删；不必为 8k 再占盘。

若以后要自己 export（须 **Linux x86_64 + RKLLM Toolkit ≥ 1.3.0**）：

```bash
# 拉上游
./export/fetch_qwen35_2b_hf.sh ./hf_src/Qwen3.5-2B

# 在已装 toolkit 的 VM 上
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

Mac 只能拉 HF；正式 `export_rkllm` 只能在 Linux。板端 `librkllmrt` 与 Toolkit 同大版本。
