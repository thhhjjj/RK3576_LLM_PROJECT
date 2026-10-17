#!/bin/bash -e
# Always install S39llm_ctrl. Copy prebuilt llm_ctrl.ko into rootfs when present.

TARGET_DIR="${1:-$TARGET_DIR}"
SDK_DIR="$(cd "$(dirname "$0")/../../../../../.." && pwd)"
KO_SRC="$SDK_DIR/kernel-6.1/markdown/project/llm_ctrl/llm_ctrl.ko"

mkdir -p "$TARGET_DIR/lib/modules"

if [ -f "$KO_SRC" ]; then
	cp -f "$KO_SRC" "$TARGET_DIR/lib/modules/llm_ctrl.ko"
	echo ">>> llm_ctrl: installed $KO_SRC -> /lib/modules/llm_ctrl.ko"
else
	echo ">>> llm_ctrl: WARN no $KO_SRC (build .ko before rootfs for auto-load)" >&2
fi

exit 0
