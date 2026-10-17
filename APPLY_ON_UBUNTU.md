# Ubuntu：落地 makediff（速查）

完整说明见同目录 [README.md](./README.md)。

在 SDK **根目录**：

```bash
chmod +x makediff/git_push_must_change.sh
./makediff/git_push_must_change.sh makediff
```

**验收 SDK（不是 makediff）**：

```bash
grep DEPENDENCIES buildroot/package/llm_panel/llm_panel.mk
# → lv_drivers freetype ...

ls kernel-6.1/markdown/project/llm_panel/evdev.c
ls -lh buildroot/dl/lvgl/lvgl-8.3.0.tar.gz
```

再编：

```bash
./build.sh lunch    # 选 5 kickpi_k7c_buildroot
./build.sh rootfs
```
