#!/usr/bin/env bash
# 拉取上游 Qwen/Qwen3.5-2B（Transformers 原权重）到本目录旁 hf_src/
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="${1:-$ROOT/hf_src/Qwen3.5-2B}"
REPO="${HF_REPO:-Qwen/Qwen3.5-2B}"
# 国内可：HF_ENDPOINT=https://hf-mirror.com
export HF_ENDPOINT="${HF_ENDPOINT:-https://hf-mirror.com}"

mkdir -p "$(dirname "$DEST")"
echo "snapshot_download $REPO → $DEST (endpoint=$HF_ENDPOINT)"

PY=""
if [ -x "$ROOT/../.venv-hf/bin/python" ]; then
	PY="$ROOT/../.venv-hf/bin/python"
elif [ -x "$ROOT/.venv-hf/bin/python" ]; then
	PY="$ROOT/.venv-hf/bin/python"
elif command -v python3 >/dev/null; then
	PY=python3
fi

"$PY" - <<PY
from huggingface_hub import snapshot_download
import os
dest = r"""$DEST"""
repo = r"""$REPO"""
path = snapshot_download(
    repo_id=repo,
    local_dir=dest,
    local_dir_use_symlinks=False,
    resume_download=True,
)
print("OK", path)
PY

ls -lh "$DEST" | head
test -f "$DEST/config.json" && echo "config.json OK"
