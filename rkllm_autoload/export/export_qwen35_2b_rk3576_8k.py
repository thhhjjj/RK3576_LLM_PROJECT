#!/usr/bin/env python3
"""
在 **Linux + RKLLM Toolkit ≥ 1.3.0** 上运行（Mac 无 CUDA/toolkit 装不了正式导出）。

上游：Qwen/Qwen3.5-2B（HF Transformers）
目标：RK3576 · W4A16 · max_context=8192 · 语言侧 .rkllm

用法示例：
  export CUDA_VISIBLE_DEVICES=0
  python3 export_qwen35_2b_rk3576_8k.py \\
    --model /path/to/Qwen3.5-2B \\
    --out /path/to/QWEN3.5_2B_8k.rkllm \\
    --dataset /path/to/rknn-llm/examples/rkllm_api_demo/export/data_quant.json
"""
from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path


def main() -> int:
	ap = argparse.ArgumentParser()
	ap.add_argument("--model", required=True, help="HF 模型目录（含 config.json）")
	ap.add_argument("--out", required=True, help="输出 .rkllm 路径")
	ap.add_argument(
		"--dataset",
		default="",
		help="量化校准 json（官方 data_quant.json）；可空则走 toolkit 默认",
	)
	ap.add_argument("--device", default="cuda", choices=["cuda", "cpu"])
	ap.add_argument("--dtype", default="float16", choices=["float16", "bfloat16", "float32"])
	ap.add_argument(
		"--quantized-dtype",
		default="W4A16",
		help="社区包名为 w4a16_g128；toolkit 侧常用 W4A16 + grq",
	)
	ap.add_argument(
		"--quantized-algorithm",
		default="grq",
		help="w4a16 官方推荐 grq（normal 也可试）",
	)
	ap.add_argument("--max-context", type=int, default=8192)
	ap.add_argument("--num-npu-core", type=int, default=2)
	ap.add_argument("--platform", default="rk3576")
	args = ap.parse_args()

	model = Path(args.model).expanduser().resolve()
	out = Path(args.out).expanduser().resolve()
	if not (model / "config.json").is_file():
		print(f"ERROR: missing {model}/config.json", file=sys.stderr)
		return 2

	try:
		from rkllm.api import RKLLM
	except ImportError:
		print(
			"ERROR: 未找到 rkllm.api。请在已安装 RKLLM Toolkit ≥1.3.0 的 Linux 环境运行。",
			file=sys.stderr,
		)
		return 3

	os.environ.setdefault("CUDA_VISIBLE_DEVICES", "0")
	print(f"load_huggingface: {model} device={args.device} dtype={args.dtype}")
	llm = RKLLM()
	ret = llm.load_huggingface(
		model=str(model),
		model_lora=None,
		device=args.device,
		dtype=args.dtype,
		custom_config=None,
		load_weight=True,
	)
	if ret != 0:
		print(f"load_huggingface failed: {ret}", file=sys.stderr)
		return ret

	dataset = args.dataset or None
	if dataset:
		dataset = str(Path(dataset).expanduser().resolve())
		if not Path(dataset).is_file():
			print(f"ERROR: dataset not found: {dataset}", file=sys.stderr)
			return 2

	print(
		f"build: platform={args.platform} quant={args.quantized_dtype}/"
		f"{args.quantized_algorithm} max_context={args.max_context}"
	)
	ret = llm.build(
		do_quantization=True,
		optimization_level=1,
		quantized_dtype=args.quantized_dtype,
		quantized_algorithm=args.quantized_algorithm,
		target_platform=args.platform,
		num_npu_core=args.num_npu_core,
		extra_qparams=None,
		dataset=dataset,
		hybrid_rate=0,
		max_context=args.max_context,
	)
	if ret != 0:
		print(f"build failed: {ret}", file=sys.stderr)
		return ret

	out.parent.mkdir(parents=True, exist_ok=True)
	print(f"export_rkllm → {out}")
	ret = llm.export_rkllm(str(out))
	if ret != 0:
		print(f"export_rkllm failed: {ret}", file=sys.stderr)
		return ret

	print("OK", out, "size_MB", round(out.stat().st_size / 1024 / 1024, 1))
	return 0


if __name__ == "__main__":
	sys.exit(main())
