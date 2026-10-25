# mali_autoload — PHASE6 GPU 用户态暂存

板上已有内核 GPU（`/dev/dri/renderD128`、`27800000.gpu`）。本目录仿 `rkllm_autoload`：so 由 `overlays/30-llm_ctrl/prepare.sh` 装进 `/usr/lib/`。

## 当前清单（2026-10-24）

来源：完整 `rockchip_rk3576` target（G52 wayland-gbm）。

| 文件 | 说明 |
|---|---|
| `libmali.so.1.9.0` + 软链 / hook | 真库 ~41MB |
| `libmali-bifrost-g52-g13p0-wayland-gbm.so` | → `libmali.so.1` |
| `libEGL` / `libGLESv2` / `libGLESv1_CM` / `libgbm` | stub ~5.6KB |
| `libwayland-client.so.0.22.0` + 软链 | mali NEEDED |
| `libwayland-server.so.0.22.0` + 软链 | mali NEEDED |

`.so` **不进 git**（见 `.gitignore`）；只提交本 README。

## 同步与刷机

```bash
# SDK 根（SMB 后 makediff 已含上述 so）
bash makediff/git_push_must_change.sh makediff
./build.sh rootfs
# 日志：>>> mali: installed libwayland-… / libmali-…
```

快测可不刷整包，直接 `adb push` wayland + 重启 panel。

```bash
adb shell 'ls -l /usr/lib/libmali* /usr/lib/libEGL* /usr/lib/libgbm* /usr/lib/libwayland-*'
```

## 从完整 SDK 补缺（若丢文件）

```bash
FULL=buildroot/output/rockchip_rk3576/target/usr/lib
cp -a "$FULL"/libmali*.so* "$FULL"/libEGL.so* \
      "$FULL"/libGLESv*.so* "$FULL"/libgbm.so* \
      "$FULL"/libwayland-client.so* "$FULL"/libwayland-server.so* \
      makediff/mali_autoload/
```

`readelf -d libmali.so.1.9.0 | grep NEEDED` 须能满足：至少 `libwayland-client` + `libwayland-server`（`libdrm`/`libffi` 等 llm rootfs 通常已有）。
