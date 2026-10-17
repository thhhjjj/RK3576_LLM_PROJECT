# RK3576 LLM Project — 补丁 / 差分管理

本地保留完整 RK3576 Linux SDK（约 20GB）。**日常改动提交在差分包**，不要推整库。

| 远程 / 目录 | 用途 |
|-------------|------|
| **makediff/**（或 `origin` = [RK3576_LLM_PROJECT](https://github.com/thhhjjj/RK3576_LLM_PROJECT)） | **当前主流程**：overlay 差分，Ubuntu 用脚本落地 |
| `private-sdk` | 完整 SDK 备份 |
| `patches/` + `gen_patches.sh` | 可选：长期补丁序列（与 makediff 互补） |
| 基线标签 `sdk-baseline` | 干净 SDK 快照 |

## 当前推荐流程（PHASE1）

1. **改代码**（Mac SDK 或直接改 makediff 内同名路径）
2. **同步进 `Desktop/makediff`**（相对路径与 SDK 一致）
3. **在 makediff / 轻量仓 git commit & push**
4. Ubuntu SDK 根目录落地并编译：

```bash
./makediff/git_push_must_change.sh makediff
./build.sh lunch    # 5 = kickpi_k7c_buildroot
./build.sh rootfs   # → buildroot/output/rockchip_rk3576_kickpi_k7c_llm/
```

完整说明、验收命令、已知坑：见 **`makediff/README.md`**（速查：`makediff/APPLY_ON_UBUNTU.md`）。

## 可选：补丁序列流程

适合需要 `patches/series` 可重放的场景：

```bash
./scripts/gen_patches.sh kernel kernel-6.1
./scripts/gen_patches.sh device device
./scripts/apply_patches.sh --check
```

干净树恢复：`git checkout sdk-baseline && ./scripts/apply_patches.sh`。

## 脚本入口

| 脚本 | 作用 |
|------|------|
| `makediff/git_push_must_change.sh` | overlay → SDK（主入口） |
| `scripts/git_push_must_change.sh` | 同上（副本） |
| `scripts/gen_patches.sh` / `apply_patches.sh` | 补丁导出 / 应用 |
