#!/usr/bin/env bash
# git_push_must_change.sh
#
# 从轻量仓库 RK3576_LLM_PROJECT 拉取「必须改动」文件，并补丁进当前 SDK 根目录。
# 旧文件备份到 lastonce/<相对路径>，保留目录结构。
#
# 用法（在 SDK 根目录执行）：
#   ./scripts/git_push_must_change.sh              # 拉取 origin 最新后应用
#   ./scripts/git_push_must_change.sh /path/overlay  # 用已有目录（如 makediff）应用，不联网
#   REPO_URL=... BRANCH=main BACKUP=lastonce ./scripts/git_push_must_change.sh
#
# 环境变量：
#   REPO_URL   默认 https://github.com/thhhjjj/RK3576_LLM_PROJECT.git
#   BRANCH     默认 main
#   BACKUP     默认 lastonce
#   CACHE_DIR  默认 .must_change_repo（相对 SDK 根）

set -euo pipefail

REPO_URL="${REPO_URL:-https://github.com/thhhjjj/RK3576_LLM_PROJECT.git}"
BRANCH="${BRANCH:-main}"
BACKUP="${BACKUP:-lastonce}"
CACHE_DIR="${CACHE_DIR:-.must_change_repo}"

die() { echo "错误: $*" >&2; exit 1; }
info() { echo "$*"; }

# 定位 SDK 根：含 kernel-6.1 或 kernel 软链
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

# 是否跳过该相对路径
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
		# 仅文档/工具树，不往内核树里盖
		markdown/*|*/markdown/*) return 0 ;;
	esac
	return 1
}

# 收集 overlay 里要落地的文件（相对 SRC）——兼容 bash 3.2（无 mapfile）
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

	# 仓库根下的同名脚本 → SDK scripts/
	dest="$f"
	if [ "$(basename "$f")" = "git_push_must_change.sh" ]; then
		dest="scripts/git_push_must_change.sh"
	fi

	if [ -e "$SDK_ROOT/$dest" ]; then
		mkdir -p "$SDK_ROOT/$BACKUP/$(dirname "$dest")"
		mv -v "$SDK_ROOT/$dest" "$SDK_ROOT/$BACKUP/$dest"
	fi
	mkdir -p "$SDK_ROOT/$(dirname "$dest")"
	cp -v "$SRC/$f" "$SDK_ROOT/$dest"
	if [ "$(basename "$dest")" = "git_push_must_change.sh" ]; then
		chmod +x "$SDK_ROOT/$dest"
	fi
	APPLIED=$((APPLIED + 1))
done < "$FILE_LIST"

info ""
info "完成: 应用 ${APPLIED} 个文件，跳过 ${SKIPPED} 个。"
info "备份目录: $SDK_ROOT/$BACKUP/"
info "下一步示例: ./build.sh kernel"
