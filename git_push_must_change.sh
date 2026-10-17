#!/usr/bin/env bash
# git_push_must_change.sh
#
# 把「差分包」落到完整 SDK。日常提交在 makediff / RK3576_LLM_PROJECT，
# 本脚本只负责拷贝进编译树（旧文件进 lastonce/）。用法详见 makediff/README.md。
#
# 用法（在 SDK 根目录执行）：
#   ./makediff/git_push_must_change.sh makediff       # 推荐：本地 overlay
#   ./scripts/git_push_must_change.sh /path/overlay
#   ./makediff/git_push_must_change.sh                 # 无参：git 拉轻量仓再应用
#
# 会落地（overlay 里有则拷）：
#   - device / kernel DTS·defconfig·FTS
#   - buildroot 裁剪、package/llm_panel、overlays/30-llm_ctrl
#   - buildroot/dl/lvgl、lv_drivers 离线 tarball
#   - kernel-6.1/markdown/project/llm_ctrl、llm_panel（跳过 *.md）
#   - llm_ctrl_autoload/llm_ctrl.ko → project/llm_ctrl/
#
# 环境变量：REPO_URL BRANCH BACKUP CACHE_DIR

set -euo pipefail

REPO_URL="${REPO_URL:-https://github.com/thhhjjj/RK3576_LLM_PROJECT.git}"
BRANCH="${BRANCH:-main}"
BACKUP="${BACKUP:-lastonce}"
CACHE_DIR="${CACHE_DIR:-.must_change_repo}"

die() { echo "错误: $*" >&2; exit 1; }
info() { echo "$*"; }

find_sdk_root() {
	local d="$1"
	while [ "$d" != "/" ]; do
		if [ -d "$d/kernel-6.1" ] || [ -e "$d/kernel" ]; then
			echo "$d"
			return 0
		fi
		d="$(dirname "$d")"
	done
	return 1
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
START_DIR="$(pwd)"

SDK_ROOT="$(find_sdk_root "$START_DIR" || true)"
if [ -z "${SDK_ROOT:-}" ]; then
	SDK_ROOT="$(find_sdk_root "$SCRIPT_DIR" || true)"
fi
[ -n "${SDK_ROOT:-}" ] || die "请在 SDK 根目录执行（需存在 kernel-6.1 或 kernel）"
cd "$SDK_ROOT"
info "SDK 根目录: $SDK_ROOT"

SRC="${1:-}"
if [ -n "$SRC" ]; then
	[ -d "$SRC" ] || die "目录不存在: $SRC"
	SRC="$(cd "$SRC" && pwd)"
	info "使用本地 overlay: $SRC"
else
	CACHE_PATH="$SDK_ROOT/$CACHE_DIR"
	if [ -d "$CACHE_PATH/.git" ]; then
		info "更新 $CACHE_PATH ($BRANCH) ..."
		git -C "$CACHE_PATH" fetch origin
		git -C "$CACHE_PATH" checkout "$BRANCH"
		git -C "$CACHE_PATH" pull --ff-only origin "$BRANCH"
	else
		info "克隆 $REPO_URL -> $CACHE_PATH ..."
		rm -rf "$CACHE_PATH"
		git clone --depth 1 -b "$BRANCH" "$REPO_URL" "$CACHE_PATH"
	fi
	SRC="$CACHE_PATH"
fi

# return 0 = skip, 1 = apply
should_skip() {
	local rel="$1"
	case "$rel" in
		.git|.git/*) return 0 ;;
		.gitignore) return 0 ;;
		.DS_Store|*/.DS_Store) return 0 ;;
		*.md|*.MD) return 0 ;;
		*.patch) return 0 ;;
		APPLY_*|*/APPLY_*) return 0 ;;
		git_push_must_change.sh|*/git_push_must_change.sh) return 0 ;;
		# staging helper dir: handled specially below
		llm_ctrl_autoload|llm_ctrl_autoload/*) return 0 ;;
	esac

	# Allow PHASE1 out-of-tree apps (C/Makefile/CMake), skip other markdown docs
	case "$rel" in
		kernel-6.1/markdown/project/llm_ctrl/*|\
		kernel/markdown/project/llm_ctrl/*|\
		kernel-6.1/markdown/project/llm_panel/*|\
		kernel/markdown/project/llm_panel/*)
			return 1 ;;
		markdown/*|*/markdown/*)
			return 0 ;;
	esac
	return 1
}

apply_one() {
	local src_rel="$1"
	local dest="$2"
	local mode="${3:-}"

	[ -f "$SRC/$src_rel" ] || return 0

	if [ -e "$SDK_ROOT/$dest" ]; then
		mkdir -p "$SDK_ROOT/$BACKUP/$(dirname "$dest")"
		mv -v "$SDK_ROOT/$dest" "$SDK_ROOT/$BACKUP/$dest"
	fi
	mkdir -p "$SDK_ROOT/$(dirname "$dest")"
	cp -v "$SRC/$src_rel" "$SDK_ROOT/$dest"
	case "$mode" in
		+x) chmod +x "$SDK_ROOT/$dest" ;;
	esac
	APPLIED=$((APPLIED + 1))
}

FILE_LIST="$(mktemp)"
trap 'rm -f "$FILE_LIST"' EXIT
(
	cd "$SRC"
	find . -type f \
		! -path './.git/*' \
		\( -path './kernel-6.1/*' -o -path './kernel/*' -o -path './device/*' \
		   -o -path './u-boot/*' -o -path './buildroot/*' -o -path './external/*' \
		   -o -path './scripts/*' \) \
		-print 2>/dev/null
	[ -f ./git_push_must_change.sh ] && echo ./git_push_must_change.sh
) | sed 's|^\./||' | sort -u > "$FILE_LIST"

[ -s "$FILE_LIST" ] || die "overlay 中没有可应用的文件: $SRC"

APPLIED=0
SKIPPED=0
while IFS= read -r f; do
	[ -n "$f" ] || continue
	if should_skip "$f"; then
		SKIPPED=$((SKIPPED + 1))
		continue
	fi
	[ -f "$SRC/$f" ] || continue

	dest="$f"
	mode=""
	if [ "$(basename "$f")" = "git_push_must_change.sh" ]; then
		dest="scripts/git_push_must_change.sh"
		mode="+x"
	fi
	case "$dest" in
		*/etc/init.d/*|*/prepare.sh|scripts/*.sh)
			mode="+x"
			;;
	esac

	apply_one "$f" "$dest" "$mode"
done < "$FILE_LIST"

# Optional: prebuilt module for rootfs prepare.sh
if [ -f "$SRC/llm_ctrl_autoload/llm_ctrl.ko" ]; then
	info "安装预编译 llm_ctrl.ko → markdown/project/llm_ctrl/"
	apply_one "llm_ctrl_autoload/llm_ctrl.ko" \
		"kernel-6.1/markdown/project/llm_ctrl/llm_ctrl.ko" ""
fi
if [ -f "$SRC/llm_ctrl_autoload/S39llm_ctrl" ]; then
	# also keep a copy under overlay path if not already from buildroot/
	:
fi

info ""
info "完成: 应用 ${APPLIED} 个文件，跳过 ${SKIPPED} 个。"
info "备份目录: $SDK_ROOT/$BACKUP/"
info "用法说明: makediff/README.md（若 overlay 在旁边）"
info ""
info "请确认 SDK（不是 overlay）："
info "  grep DEPENDENCIES buildroot/package/llm_panel/llm_panel.mk   # 须含 lv_drivers freetype"
info "  grep 'select BR2_PACKAGE_LVGL_USE' buildroot/package/llm_panel/Config.in || echo OK"
info "  ls kernel-6.1/markdown/project/llm_panel/{main.c,evdev.c}"
info "  ls buildroot/dl/lvgl/lvgl-8.3.0.tar.gz buildroot/dl/lv_drivers/lv_drivers-8.3.0.tar.gz"
info "  grep RK_BUILDROOT_BASE_CFG device/rockchip/.chips/rk3576/rockchip_rk3576_kickpi_k7c_buildroot_defconfig"
info ""
info "下一步: ./build.sh lunch  # 选 kickpi_k7c_buildroot(5)"
info "       ./build.sh rootfs  # → buildroot/output/rockchip_rk3576_kickpi_k7c_llm/"
