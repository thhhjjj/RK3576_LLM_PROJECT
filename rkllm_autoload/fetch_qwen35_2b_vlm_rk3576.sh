#!/usr/bin/env bash
# Download Qwen3.5-2B VLM RK3576 pair into models/
# Prefers: hf / huggingface-cli; else curl (tries hf-mirror then huggingface.co).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
DEST="$ROOT/models"
mkdir -p "$DEST"

REPO="HanzoHuang/Qwen3.5-2B-RKLLM"
LLM_NAME="Qwen3.5-2B_RK3576_w4a16_g128.rkllm"
VIS_NAME="Qwen3.5-2B_vision_RK3576.rknn"
LLM_REL="RK3576/${LLM_NAME}"
VIS_REL="RK3576/${VIS_NAME}"

# Override: HF_ENDPOINT=https://huggingface.co ./fetch_...
HF_ENDPOINT="${HF_ENDPOINT:-https://hf-mirror.com}"

echo "Downloading ${REPO} (endpoint: ${HF_ENDPOINT}) …"
echo "  → ${DEST}/QWEN3.5_2B.rkllm"
echo "  → ${DEST}/vision.rknn"

download_curl() {
	local url="$1" out="$2"
	echo "curl -L --progress-bar $url"
	# -C - resume; long timeouts for multi-GB
	curl -fL -C - --connect-timeout 30 --retry 5 --retry-delay 3 \
		--progress-bar -o "$out" "$url"
}

URL_LLM="${HF_ENDPOINT}/${REPO}/resolve/main/${LLM_REL}"
URL_VIS="${HF_ENDPOINT}/${REPO}/resolve/main/${VIS_REL}"

if command -v hf >/dev/null 2>&1; then
	export HF_ENDPOINT
	TMP="${TMPDIR:-/tmp}/qwen35-2b-rkllm-$$"
	mkdir -p "$TMP"
	hf download "$REPO" "$LLM_REL" "$VIS_REL" --local-dir "$TMP"
	LLM="$(find "$TMP" -name "$LLM_NAME" | head -n1)"
	VIS="$(find "$TMP" -name "$VIS_NAME" | head -n1)"
	cp -f "$LLM" "$DEST/QWEN3.5_2B.rkllm"
	cp -f "$VIS" "$DEST/vision.rknn"
	rm -rf "$TMP"
elif command -v huggingface-cli >/dev/null 2>&1; then
	export HF_ENDPOINT
	TMP="${TMPDIR:-/tmp}/qwen35-2b-rkllm-$$"
	mkdir -p "$TMP"
	huggingface-cli download "$REPO" "$LLM_REL" "$VIS_REL" --local-dir "$TMP"
	LLM="$(find "$TMP" -name "$LLM_NAME" | head -n1)"
	VIS="$(find "$TMP" -name "$VIS_NAME" | head -n1)"
	cp -f "$LLM" "$DEST/QWEN3.5_2B.rkllm"
	cp -f "$VIS" "$DEST/vision.rknn"
	rm -rf "$TMP"
else
	echo "（无 hf CLI，curl 直拉；可断点续传）"
	# remove tiny/failed leftovers so -C - works cleanly
	for f in "$DEST/QWEN3.5_2B.rkllm" "$DEST/vision.rknn"; do
		if [ -f "$f" ] && [ "$(wc -c < "$f")" -lt 1000000 ]; then
			rm -f "$f"
		fi
	done
	download_curl "$URL_LLM" "$DEST/QWEN3.5_2B.rkllm"
	download_curl "$URL_VIS" "$DEST/vision.rknn"
fi

ls -lh "$DEST/QWEN3.5_2B.rkllm" "$DEST/vision.rknn"
echo "OK → prepare.sh 会装到 /usr/share/rkllm/{QWEN3.5_2B.rkllm,vision.rknn}"
