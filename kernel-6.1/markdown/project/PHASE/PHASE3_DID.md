# PHASE3 已做记录

> 对照 [PHASE3.md](./PHASE3.md)。WiFi/WS 教程：[wifi_ws.md](./wifi_ws.md)。  
> **主线下一刀**：[PHASE6.md](./PHASE6.md) GPU → [PHASE7.md](./PHASE7.md) 体验；**添头**：[PHASE5.md](./PHASE5.md) frpc（[PHASE4](./PHASE4.md) BLE **已取消**）。

## 进度总览

| 步骤块 | 状态 |
|---|---|
| 1. 扇出总线 | **已过** |
| 2. WebSocket | **已过**（板测流式） |
| 3. WS → 本地推理 | **已过** |
| 4. DeepSeek SSE + 模式切换 | **代码已过（0.2.1）**；**板测待勾**（需 Key + 编新 ELF） |
| 5. Key / 安全收口 | **代码侧已做**：Key 仅内存、栈上 wipe、不打日志；冷启动/板测 grep 待勾 |
| 6. 不做 GATT/frpc/拼音 | **保持** |

桌面旧 `llm_panel`（约 55KB / 05:16）仍是 **0.1.x**，**必须重编 0.2.1** 才能测云端。

---

## 版本与路径

| 项 | 值 |
|---|---|
| UI | **`vtest：0.2.1`**（SSE abort 加速 + 云端跳过压缩） |
| WS | `0.0.0.0:8765` |
| 云端 | `https://api.deepseek.com` + `deepseek-chat` |
| Key | `DEEPSEEK_API_KEY` / `LLM_PANEL_API_KEY` → 内存；`cloud_sse_fini` / 用后 `memset` |
| 源码 | `cloud_sse.c`、`ws_hub.c`、`main.c`（`panel_ask`） |

---

## 完成线七条（对照 PHASE3.md）

| # | 要求 | 判定 |
|---|---|---|
| 1 | 局域网 WS 流式 | **已过** |
| 2 | 慢客户端 / 断连隔离 | **已过**（每连接队列；满丢 token） |
| 3 | DeepSeek SSE + 错误态 | **代码过 / 板测待** |
| 4 | Key 仅内存 | **代码过 / 板测 grep 待** |
| 5 | 切换先 abort 再改路 | **代码过** |
| 6 | 三路扇出骨架 UI+WS（BLE 空） | **已过**（BLE→PHASE4） |
| 7 | 不开 GATT/frpc/解析远端 | **已过** |

---

## 板测口令（0.2.1）

```bash
# VM
bash makediff/git_push_must_change.sh makediff
rm -f kernel-6.1/markdown/project/llm_panel/Makefile
OUT=buildroot/output/rockchip_rk3576_kickpi_k7c_llm
make -C buildroot O=$PWD/$OUT llm_panel-dirclean llm_panel-rebuild
strings $OUT/target/usr/bin/llm_panel | grep '0\.2\.1'
cp -v $OUT/target/usr/bin/llm_panel ~/Desktop/

# Mac（Key 勿提交 git；用完可 unset）
adb push ~/Desktop/llm_panel /usr/bin/llm_panel
adb shell 'chmod 755 /usr/bin/llm_panel; killall llm_panel; sleep 1
DEEPSEEK_API_KEY=你的key /usr/bin/llm_panel >/tmp/llm_panel.log 2>&1 &'

adb shell "strings /usr/bin/llm_panel | grep 0.2.1"
adb shell 'grep -iE "sk-|Bearer|DEEPSEEK" /tmp/llm_panel.log || echo OK_no_key_in_log'
```

屏：拨「云端」→ 试问流式；拨回「本地」应停云端；断网再问应有错误/`finish` 而非死转圈。

---

## 结论

- **代码**：PHASE3 步骤 1–5 主体已落地。  
- **你这边**：编并推 **0.2.1** → 云端板测 → 勾上表「板测待」→ 再开 [PHASE4.md](./PHASE4.md) GATT。
