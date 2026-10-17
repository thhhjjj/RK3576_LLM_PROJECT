# RK3576 LLM — 差分包（makediff）使用说明

> 同步状态（2026-10-17）：PHASE1 面板/中文字体/裁剪 Buildroot/`llm_ctrl` 等已齐。明细见 `kernel-6.1/markdown/project/PHASE/PHASE1_DID.md`。

日常**只在本目录（或云端轻量仓 `RK3576_LLM_PROJECT`）提交**。完整 SDK（~20GB）不当日常推送目标。

本目录是相对 KickPi K7C Buildroot SDK 的 **overlay**：保持与 SDK 相同的相对路径，用脚本一键落到 Ubuntu 编译机。

| 角色 | 路径 |
|------|------|
| 差分源 / 提交处 | `makediff/` 或 GitHub `thhhjjj/RK3576_LLM_PROJECT` |
| 编译 SDK | Ubuntu：`~/rk3576/rk3576_sdk/rk3576-linux` |
| 落地脚本 | `git_push_must_change.sh`（本目录或 `scripts/`） |

---

## 1. 目录里有什么

```
makediff/
├── git_push_must_change.sh          # 落地脚本（主入口）
├── README.md / APPLY_ON_UBUNTU.md   # 说明（*.md 不会拷进 SDK）
├── device/...                       # 板级 defconfig（指向 k7c_llm）
├── buildroot/
│   ├── configs/...llm*              # 裁剪产品配置
│   ├── package/llm_panel/           # Buildroot 包
│   ├── board/.../overlays/30-llm_ctrl/  # S39llm_ctrl / S90llm_panel
│   └── dl/lvgl|lv_drivers/*.tar.gz  # 离线源码包（VM 无 GitHub 时用）
├── kernel-6.1/
│   ├── arch/...                     # DTS / defconfig / FTS 修复
│   └── markdown/project/
│       ├── llm_ctrl/                # 温控 ko + nl_watch（*.md 跳过）
│       └── llm_panel/               # LVGL 面板源码
├── llm_ctrl_autoload/llm_ctrl.ko    # 预编译模块 → 编 rootfs 时装入
└── scripts/git_push_must_change.sh  # 同脚本副本
```

脚本**会跳过**：`*.md`、`APPLY_*`、`.git`、本脚本自身（用 makediff 里的直接跑即可）。

---

## 2. Mac：改代码 → 提交差分

1. 在 Mac 的 SDK 或本目录改文件后，**确保同步进 `Desktop/makediff` 对应路径**（相对路径一致）。
2. 在 makediff 所在 git 仓提交并推送（不要推整棵 SDK）。
3. 大文件：`buildroot/dl/lvgl|lv_drivers/*.tar.gz` 一并纳入差分仓（主 SDK 的 `buildroot/.gitignore` 会忽略 `/dl`）。

---

## 3. Ubuntu：落地 → 编译

在 **SDK 根目录**（含 `kernel-6.1/`）：

```bash
# 把最新 makediff 放到 SDK 下（共享盘拷贝 / git clone 均可）
# 然后：
chmod +x makediff/git_push_must_change.sh
./makediff/git_push_must_change.sh makediff
```

其它用法：

```bash
# 从 GitHub 轻量仓拉取再应用（不传 overlay 参数）
./makediff/git_push_must_change.sh
# 或
./scripts/git_push_must_change.sh

# 指定任意 overlay 路径
./makediff/git_push_must_change.sh /path/to/overlay
```

环境变量（可选）：`REPO_URL` `BRANCH` `BACKUP`（默认备份目录 `lastonce/`）`CACHE_DIR`。

### 落地后验收（必看内容，不要只看 makediff）

```bash
grep RK_BUILDROOT_BASE_CFG \
  device/rockchip/.chips/rk3576/rockchip_rk3576_kickpi_k7c_buildroot_defconfig
# 期望含 rk3576_kickpi_k7c_llm

grep DEPENDENCIES buildroot/package/llm_panel/llm_panel.mk
# 期望: lvgl lv_drivers freetype libdrm libevdev
# 注意是 lv_drivers（下划线），不是 lv-drivers

grep 'select BR2_PACKAGE_LVGL_USE' buildroot/package/llm_panel/Config.in || echo OK_no_select
# 不应再 select choice 符号（会触发 Kconfig recursive dependency）

ls kernel-6.1/markdown/project/llm_panel/{main.c,evdev.c}
ls -lh buildroot/dl/lvgl/lvgl-8.3.0.tar.gz \
       buildroot/dl/lv_drivers/lv_drivers-8.3.0.tar.gz
```

### 编译

```bash
./build.sh lunch    # 选 5：kickpi_k7c_buildroot
./build.sh rootfs   # 产出 buildroot/output/rockchip_rk3576_kickpi_k7c_llm/
```

只重编面板：

```bash
make -C buildroot O=output/rockchip_rk3576_kickpi_k7c_llm \
  llm_panel-dirclean llm_panel
./build.sh rootfs   # 再打镜像
```

改过 `Config.in` / 产品 config 后若异常，可清一次：

```bash
rm -f buildroot/output/rockchip_rk3576_kickpi_k7c_llm/.config
./build.sh rootfs
```

---

## 4. 已知坑（已在差分里修过）

| 现象 | 原因 | 差分侧对策 |
|------|------|------------|
| `No rule to make target 'lv-drivers'` | 包名是 `lv_drivers` | `llm_panel.mk` 用下划线 |
| `recursive dependency detected` | `select LVGL_USE_DRM` 撞 choice | `Config.in` 只 `depends on` |
| `lvgl` download 404 / GitHub 拒连 | VM 网络 | `buildroot/dl/...tar.gz` 离线包 |
| `undefined reference` FT_* | 未链 freetype | `CMakeLists` + DEPENDENCIES |
| `undefined reference` evdev_* | Rockchip `lv_drivers` 未编 USE_EVDEV | 自带 `llm_panel/evdev.c` |
| 中文方块 / 乱码 | 未装字库 | `llm_panel/fonts/SourceHanSansCN-Regular.otf` → `/usr/share/fonts/source-han-sans-cn/`（~8MB CN Subset） |

落地后若 SDK 里仍是旧文件，用强制拷贝再验收（见上一节 `grep`），不要只信 makediff 目录本身。

---

## 5. 刷机与板上检查

```bash
# rootfs 镜像常见位置
# buildroot/output/rockchip_rk3576_kickpi_k7c_llm/images/rootfs.ext4
# 或 SDK 汇总：output/buildroot/images/rootfs.ext4

# Mac：Loader 模式
rkdeveloptool ld
rkdeveloptool wlx rootfs /path/to/rootfs.ext4
rkdeveloptool rd
```

```bash
adb shell 'ls -l /usr/bin/llm_panel; ls /etc/init.d/S39llm_ctrl /etc/init.d/S90llm_panel; lsmod | grep llm; ps | grep llm'
```

---

## 6. 和「补丁仓」的关系

完整 SDK 仍可用 `scripts/gen_patches.sh` / `apply_patches.sh` 管理长期补丁。  
**当前 PHASE1 日常同步以本 makediff overlay 为准**：改 → 同步进 makediff → git 推轻量仓 → Ubuntu `git_push_must_change.sh` 落地 → `build.sh`。
