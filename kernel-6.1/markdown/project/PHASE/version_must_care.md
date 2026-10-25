# 版本必须当心（makediff → 板端）

> 2026-10-18 踩坑记录。改 `llm_panel` / 刷 rootfs 前先读这篇。  
> 一句话：**`makediff` 源码新 ≠ 板上是新的；以 SDK 落地源 + `target` 二进制的 `strings` 为准。**

## 版本链路（4 个不同的「版本」）

```text
Mac Desktop/makediff/.../ui.c     ← Cursor 改这里
    → SMB → VM makediff/...
    → git_push → SDK kernel-6.1/markdown/project/llm_panel/   ← Buildroot SITE
    → 编译 → …/target/usr/bin/llm_panel
    → 打包 → rootfs.ext4
    → 刷机 → 板上 /usr/bin/llm_panel
```

任一层卡住，都会出现「我明明改过 / 明明刷过，屏上还是旧的」。

## 已踩过的坑

| 现象 | 真实原因 |
|---|---|
| 板上无 `loading model`，仍是 ~22KB 旧 panel | 源码已新，但 `./build.sh rootfs` **没重编** `llm_panel`（构建目录甚至不存在） |
| `makediff/ui.c` 有 `vtest`，镜像 / 板上没有 | **`git_push` 没成功落地**到 SDK；或落地后没 `dirclean`/`rebuild` |
| `./makediff/git_push_must_change.sh` → `Permission denied` | SMB 共享常丢掉执行位，脚本根本没跑 |
| 反复刷机，板上 md5 不变 | 刷的 ext4 里二进制就没变（旧包），不是显示问题 |
| Finder 显示文件夹还是 17 号晚 | **目录 mtime 不随文件内容更新**；看具体文件时间（如 `ui.c`） |
| 镜像很大（~2.8G）以为一定是新 panel | 体积只说明塞了什么（可能含模型），**与 panel 新旧无关** |
| 标题后加版本号看不见 | 与右侧「模型: …」**叠字**；版本标应单独放（如右下角） |

## 正确流程（改 panel 后）

在 SDK 根目录（`rk3576-linux`）：

```bash
# 1) 落地 — 用 bash，避免 SMB 无 +x
bash makediff/git_push_must_change.sh makediff

# 2) 确认「编译用的」源码（不是只看 makediff）
grep -n 'vtest' kernel-6.1/markdown/project/llm_panel/ui.c
# 或其它你刚改的特征字符串

# 3) 强制重编 panel（不要只跑 build.sh rootfs）
OUT=buildroot/output/rockchip_rk3576_kickpi_k7c_llm
make -C buildroot O=$PWD/$OUT llm_panel-dirclean llm_panel-rebuild

# 4) 确认产物（没有 → 停，别打包）
strings $OUT/target/usr/bin/llm_panel | grep vtest

# 5) 再打镜像
./build.sh rootfs
cp -v $OUT/images/rootfs.ext4 ~/Desktop/
```

刷完板上复核：

```bash
adb shell "strings /usr/bin/llm_panel | grep 'vtest'"
# 或看右下角 UI：vtest：0.0.1
adb shell 'ls -l /usr/bin/llm_panel; md5sum /usr/bin/llm_panel'
```

## 提效建议

1. **小改 UI**：编出 `target/usr/bin/llm_panel` 后 `adb push` 到板，少刷整包 rootfs。  
2. **版本号每次 bump**（`0.0.1` → `0.0.2`…），屏上 / `strings` 对不上立刻停。  
3. 当前屏上标记：右下角白色 **`vtest：0.1.0`**（PHASE3：WS `:8765` 扇出；见 PHASE3_DID）。  
4. Mac 引号：整条管道包在双引号里，避免本机 `grep` 把 `model` 当文件名：
   ```bash
   adb shell "strings /usr/bin/llm_panel | grep 'loading model'"
   ```

## 收口：烤模型进 rootfs（冷启动）

模型 **不进 git**（`rkllm_autoload/.gitignore`）。Mac 放好后靠 SMB 进 VM，再被 `overlays/30-llm_ctrl/prepare.sh` 拷进镜像。

```text
Desktop/AI deal/QWEN3.5_2B.rkllm
  →（硬链）makediff/rkllm_autoload/models/QWEN3.5_2B.rkllm
  → SMB → VM 同路径
  → prepare.sh → /usr/share/rkllm/QWEN3.5_2B.rkllm
```

```bash
# VM：确认模型在编包能找到的目录（SDK 根或 makediff 兄弟）
ls -lh rkllm_autoload/models/QWEN3.5_2B.rkllm \
       makediff/rkllm_autoload/models/QWEN3.5_2B.rkllm

./build.sh rootfs
# 日志应出现：>>> rkllm: installed QWEN3.5_2B.rkllm
```

刷机后 **禁止** `adb push` so/模型/panel；按 [PHASE2_DID.md](./PHASE2_DID.md) 冷启动清单勾完成线。

## 快速自检口诀

```text
makediff 有？ → SDK kernel-6.1/... 有？ → target strings 有？ → 再刷
     ↑ git_push(bash)      ↑ dirclean+rebuild      ↑ 没有就停
```


bash makediff/git_push_must_change.sh makediff
OUT=buildroot/output/rockchip_rk3576_kickpi_k7c_llm
rm -f kernel-6.1/markdown/project/llm_panel/Makefile
make -C buildroot O=$PWD/$OUT llm_panel-dirclean llm_panel-rebuild
strings $OUT/target/usr/bin/llm_panel | grep 0.9.17
cp -v $OUT/target/usr/bin/llm_panel ~/Desktop/


adb push ~/Desktop/llm_panel /usr/bin/llm_panel
adb shell 'chmod 755 /usr/bin/llm_panel; killall llm_panel; sleep 1; /usr/bin/llm_panel >/tmp/llm_panel.log 2>&1 &'