#!/bin/bash -e
# Install llm_ctrl.ko + optional RKLLM runtime/model into rootfs.

TARGET_DIR="${1:-$TARGET_DIR}"
SDK_DIR="$(cd "$(dirname "$0")/../../../../../.." && pwd)"
KO_SRC="$SDK_DIR/kernel-6.1/markdown/project/llm_ctrl/llm_ctrl.ko"

# Prefer SDK-local staging; fall back to makediff sibling if present.
RKLLM_DIR=""
for d in \
	"$SDK_DIR/rkllm_autoload" \
	"$SDK_DIR/makediff/rkllm_autoload" \
	"$SDK_DIR/../makediff/rkllm_autoload"
do
	if [ -d "$d" ]; then
		RKLLM_DIR="$d"
		break
	fi
done

mkdir -p "$TARGET_DIR/lib/modules"

if [ -f "$KO_SRC" ]; then
	cp -f "$KO_SRC" "$TARGET_DIR/lib/modules/llm_ctrl.ko"
	echo ">>> llm_ctrl: installed $KO_SRC -> /lib/modules/llm_ctrl.ko"
else
	echo ">>> llm_ctrl: WARN no $KO_SRC (build .ko before rootfs for auto-load)" >&2
fi

# PHASE2: librkllmrt + Qwen3.5-2B VLM pair (optional until dropped in)
if [ -n "$RKLLM_DIR" ]; then
	if [ -f "$RKLLM_DIR/librkllmrt.so" ]; then
		mkdir -p "$TARGET_DIR/usr/lib"
		cp -f "$RKLLM_DIR/librkllmrt.so" "$TARGET_DIR/usr/lib/librkllmrt.so"
		echo ">>> rkllm: installed librkllmrt.so -> /usr/lib/"
	fi
	if [ -f "$RKLLM_DIR/libomp.so" ]; then
		mkdir -p "$TARGET_DIR/usr/lib"
		cp -f "$RKLLM_DIR/libomp.so" "$TARGET_DIR/usr/lib/libomp.so"
		echo ">>> rkllm: installed libomp.so -> /usr/lib/"
	fi
	# GCC OpenMP — librkllmrt NEEDED: libgomp.so.1
	if [ -f "$RKLLM_DIR/libgomp.so.1" ]; then
		mkdir -p "$TARGET_DIR/usr/lib"
		cp -f "$RKLLM_DIR/libgomp.so.1" "$TARGET_DIR/usr/lib/libgomp.so.1"
		ln -sfn libgomp.so.1 "$TARGET_DIR/usr/lib/libgomp.so"
		echo ">>> rkllm: installed libgomp.so.1 -> /usr/lib/"
	elif [ -f "$RKLLM_DIR/libgomp.so" ]; then
		mkdir -p "$TARGET_DIR/usr/lib"
		cp -f "$RKLLM_DIR/libgomp.so" "$TARGET_DIR/usr/lib/libgomp.so.1"
		ln -sfn libgomp.so.1 "$TARGET_DIR/usr/lib/libgomp.so"
		echo ">>> rkllm: installed libgomp.so as libgomp.so.1 -> /usr/lib/"
	fi

	mkdir -p "$TARGET_DIR/usr/share/rkllm"

	# On-device name matches Desktop: QWEN3.5_2B.rkllm
	RKLLM_MODEL_DST="$TARGET_DIR/usr/share/rkllm/QWEN3.5_2B.rkllm"
	if [ -f "$RKLLM_DIR/models/QWEN3.5_2B.rkllm" ]; then
		cp -f "$RKLLM_DIR/models/QWEN3.5_2B.rkllm" "$RKLLM_MODEL_DST"
		echo ">>> rkllm: installed QWEN3.5_2B.rkllm -> /usr/share/rkllm/"
	elif [ -f "$RKLLM_DIR/models/default.rkllm" ]; then
		cp -f "$RKLLM_DIR/models/default.rkllm" "$RKLLM_MODEL_DST"
		echo ">>> rkllm: installed default.rkllm as QWEN3.5_2B.rkllm"
	elif ls "$RKLLM_DIR/models/"*.rkllm >/dev/null 2>&1; then
		first="$(ls "$RKLLM_DIR/models/"*.rkllm | head -n1)"
		cp -f "$first" "$RKLLM_MODEL_DST"
		echo ">>> rkllm: installed $(basename "$first") as QWEN3.5_2B.rkllm"
	else
		echo ">>> rkllm: no .rkllm in $RKLLM_DIR/models (UI runs without model)" >&2
	fi

	# VLM vision encoder (Qwen3.5-2B pair)
	if [ -f "$RKLLM_DIR/models/vision.rknn" ]; then
		cp -f "$RKLLM_DIR/models/vision.rknn" \
			"$TARGET_DIR/usr/share/rkllm/vision.rknn"
		echo ">>> rkllm: installed vision.rknn -> /usr/share/rkllm/"
	elif ls "$RKLLM_DIR/models/"*vision*.rknn >/dev/null 2>&1; then
		first="$(ls "$RKLLM_DIR/models/"*vision*.rknn | head -n1)"
		cp -f "$first" "$TARGET_DIR/usr/share/rkllm/vision.rknn"
		echo ">>> rkllm: installed $(basename "$first") as vision.rknn"
	else
		echo ">>> rkllm: no vision.rknn (text-only until VLM path is wired)" >&2
	fi
else
	echo ">>> rkllm: no rkllm_autoload dir (optional for PHASE2)" >&2
fi

exit 0
