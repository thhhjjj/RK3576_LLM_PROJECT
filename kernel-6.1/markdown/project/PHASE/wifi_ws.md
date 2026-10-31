# WiFi + WebSocket 从零上手（RK3576 llm_panel）

> 给「只写过一点嵌入式 / 第一次碰局域网实时通信」的自己看。  
> 对应代码：`llm_panel/ws_hub.c`、`ws_hub.h`；试连页：`llm_panel/www/ws_test.html`。  
> 阶段记录：[PHASE3_DID.md](./PHASE3_DID.md)。

---

## 一、先搞清三个词

### 1. IP 地址

局域网里每台设备有一个号码，例如 `192.168.31.90`。  
**板子的 IP 和 Mac 的 IP 必须在同一网段**（前三段常一样，如都是 `192.168.31.x`），两边才能直接对话。

查板子 IP（USB 连着 adb 时）：

```bash
adb shell 'ip -4 addr show wlan0'
```

看 `inet 192.168.31.90/24` 里的 **`192.168.31.90`**。DHCP 续租后数字可能变（我们曾从 `.89` 变成 `.90`）。

查 Mac IP：系统设置 → 网络，或：

```bash
ipconfig getifaddr en0
```

### 2. 端口（port）

同一台机器上可以跑很多服务。端口用来区分「找谁」：

| 常见端口 | 谁在听 |
|---|---|
| 80 / 443 | 网页 HTTP / HTTPS |
| 22 | SSH |
| **8765** | 我们的 **llm_panel WebSocket**（写死在 `WS_HUB_PORT`） |

板上确认在听：

```bash
adb shell 'netstat -lnt | grep 8765'
```

应看到 `0.0.0.0:8765`（对所有网卡开放）。若只有 `127.0.0.1:8765`，局域网浏览器连不上。

### 3. WebSocket 是什么

- 普通网页：浏览器请求一次，服务器回一整页，连接结束（HTTP）。
- **WebSocket**：先用 HTTP「升级」成一条**长连接**，之后双方可以**随时推消息**（适合流式 token）。

地址长这样：

```text
ws://192.168.31.90:8765/
```

| 段 | 含义 |
|---|---|
| `ws://` | 明文 WebSocket（还有 `wss://` 加密，本阶段不用） |
| `192.168.31.90` | 板子 IP |
| `8765` | 端口 |
| `/` | 路径（我们用根路径） |

**重要：`ws://…` 不能贴进浏览器地址栏当网址打开。**  
地址栏只认 `http://` / `https://` / `file://`。  
正确做法：打开试连页 HTML，把 `ws://…` 填进页面里的 **URL 输入框**，再点「连接」。

---

## 二、本项目里数据怎么流

```text
┌─────────────┐     tokq_push      ┌──────────┐
│ RKLLM 回调   │ ─────────────────► │  tokq    │──► LVGL 对话框（本机屏）
│ / session   │                    │ 扇出总线  │
└─────────────┘                    └────┬─────┘
                                        │ ws_hub_fanout
                                        ▼
                               每连接发送队列（最多 4 客户端）
                                        │
                                        ▼  lws 线程 WRITEABLE
                               浏览器 WebSocket 客户端
                                        │
                                        │ 上行 {"type":"ask","text":"..."}
                                        ▼
                                   session_ask → 又回到上面
```

硬规则（PHASE3 写死的）：

1. **推理回调里绝不 `lws_write`**，只入队。  
2. 某个浏览器断了 / 慢了：只丢**它自己**队列里的旧 token，**推理和本机屏继续**。  
3. 同时只跑一路 `session_ask`；忙则回 `{"type":"error","text":"busy"}`。

### 帧格式（约定）

**浏览器 → 板：**

- 纯文本 = 问题；或  
- `{"type":"ask","text":"你的问题"}`

**板 → 浏览器：**

```json
{"type":"token","text":"片段"}
{"type":"status","text":"connected"}
{"type":"error","text":"busy"}
{"type":"finish"}
```

---

## 三、WiFi：板子怎么上网（Seekwave）

芯片是 **Seekwave SWT6621S**（不是文档里偶尔写错的 AP6256）。接口名一般是 `wlan0`。

### 1. 配置文件

路径：`/etc/wpa_supplicant.conf`

SSID 若有全角 / 特殊符号，用 **hex** 写 `ssid=`（`wpa_cli scan_results` 里能看到转义形式，再自己换成 hex）。  
家用 WPA2+WPA3 混合热点：可先试纯 `key_mgmt=WPA-PSK`（我们踩过 SAE 关联 status 40）。

### 2. 常用命令（adb shell 里）

```bash
# 扫附近热点
wpa_cli -i wlan0 scan
wpa_cli -i wlan0 scan_results

# 看是否连上（要 COMPLETED）
wpa_cli -i wlan0 status

# 本 rootfs 没有 dhclient，用 udhcpc 拿 IP
udhcpc -i wlan0 -n -q
ip -4 addr show wlan0
```

### 3. 一键重启连网（配置改好后）

```bash
adb shell 'killall wpa_supplicant udhcpc 2>/dev/null; wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant.conf'
adb shell 'sleep 4; wpa_cli -i wlan0 status'
adb shell 'udhcpc -i wlan0 -n -q'
adb shell 'ip -4 addr show wlan0'
```

### 4. 开机自动连「已保存网络」

Buildroot 无 Debian 那套 `wlan0-connect.service`。板上已加：

| 文件 | 作用 |
|---|---|
| `/etc/wpa_supplicant.conf` | **只改这一处**：写成真实 `ssid` / `psk`（勿留占位 `SSID`/`PASSWORD`） |
| `/etc/network/interfaces.d/wlan0` | `auto wlan0` + `wpa-conf` → `ifup -a` 会起连 |
| `/etc/init.d/S38wlan0-connect` | wifibt 后等接口、补 wpa/DHCP（占位账号会跳过并打日志） |

源码副本：`llm_panel/rootfs_wifi/`（刷机后可再 `adb push`）。  
改好 conf 后重启验收：`wpa_cli -i wlan0 status` 应为 `COMPLETED`，且 `ip -4 addr show wlan0` 有 `inet`。

---

## 四、五分钟验收 WebSocket（照做）

前置：屏上右下角有版本号（如 `vtest：0.1.2`），右上角有 **`WS :8765`**，模型已加载。

1. **查 IP**  
   `adb shell 'ip -4 addr show wlan0'` → 记下 `inet` 那个地址。

2. **Mac 与板同一局域网**  
   最好都连同一个 WiFi。Mac 只插网线、板只连 WiFi 时，有的路由会「互通奇怪」，出现 ping 不通 → 浏览器 `1006`。

3. **打开试连页**  
   Finder 打开：  
   `makediff/kernel-6.1/markdown/project/llm_panel/www/ws_test.html`  
   （双击用浏览器打开，这是 `file://` 页面，正常。）

4. **填 URL**（示例，以你板子为准）  

   ```text
   ws://192.168.31.90:8765/
   ```

5. 点 **连接** → 应出现 `[open]` 和 `[status] connected`。  
6. 点 **试问** → 流式出字 → `[finish]`；板上对话区同步有 `[WS] 你: …`。

---

## 五、我们踩过的坑（问题 → 原因 → 解决）

按时间线，方便以后对照。

### W1. `ws://192.168.1.100` 连不上

- **原因**：那是文档里的**占位 IP**，不是你的板。  
- **解决**：每次用 `ip -4 addr show wlan0` 看真实 IP。

### W2. 把 `ws://…` 贴进浏览器顶栏

- **原因**：浏览器地址栏不会建立 WebSocket。  
- **解决**：只打开 `ws_test.html`，在页面输入框里填 `ws://`。

### W3. WiFi 配置是 `SSID` / `PASSWORD` 占位

- **原因**：刷 rootfs 后默认假账号；`wpa_state` 一直 `SCANNING`。  
- **解决**：写成真实热点；SSID 特殊字符用 hex。

### W4. `Invalid PSK '\”….\"'`

- **原因**：从备忘录 / 富文本粘贴，密码两边变成了**弯引号** `”`，wpa 解析失败。  
- **解决**：只用 ASCII 直引号 `"`；或用 `printf` 写文件后 `cat -A` 检查有没有怪字符。

### W5. 关联失败 status 40（SAE）

- **原因**：`key_mgmt=WPA-PSK SAE` 在部分路由 / 驱动上握手失败。  
- **解决**：先改 `key_mgmt=WPA-PSK`（可选 `ieee80211w=1`），我们因此连上 Ciallo 并拿到 IP。

### W6. `dhclient: command not found`

- **原因**：Buildroot 镜像里没有 `dhclient`。  
- **解决**：用 **`udhcpc -i wlan0 -n -q`**（板上也有 `dhcpcd`）。

### W7. `[error]` / `[close] code=1006`，IP 填错成旧的

- **原因**：DHCP 后 IP 从 `.89` 变成 `.90`，页面还连旧地址。  
- **解决**：连之前再查一次 IP；1006 = 异常断开，先怀疑网络/IP，再怀疑协议。

### W8. Mac ping 不通板，但 adb 正常

- **原因**：USB 调试不依赖 WiFi；Mac 有线、板 WiFi 时二层/隔离可能导致 Mac→板 ARP 失败。  
- **解决**：Mac 改连同一 WiFi；确认 `ping 板IP` 通再开浏览器。

### W9. `adb reverse` 后只听 `127.0.0.1:8765`

- **原因**：反代占口 / 干扰，panel 或环境变成只绑回环，局域网连不上。  
- **解决**：有 WiFi 时**不要反代**；若用过：`adb reverse --remove-all`，再重启 `llm_panel`，确认 `netstat` 是 `0.0.0.0:8765`。

### W10. `adb push` 后面板起不来

- **原因**：push 后权限变成 `rw-rw-rw-`，没有执行位。  
- **解决**：

```bash
adb push ~/Desktop/llm_panel /usr/bin/llm_panel
adb shell 'chmod 755 /usr/bin/llm_panel; killall llm_panel; sleep 1; /usr/bin/llm_panel >/tmp/llm_panel.log 2>&1 &'
```

### W11. 握手 101 了但马上断开 / 浏览器连不上（协议）

- **原因**：浏览器常**不带** `Sec-WebSocket-Protocol`；若 lws 的 `protocols[0]` 是纯 HTTP，连接派错协议，adopt 后十几毫秒就关。  
- **解决（0.1.2）**：`protocols[0]` 就是 `"llm"` + `callback_llm`；试页仍可 `new WebSocket(url, ['llm'])`。

### W12. 已 connected，ask 后浏览器收不到 token

- **原因**：其它线程 `fanout` 只 `lws_cancel_service`，没有给各 `wsi` 调 `lws_callback_on_writable`。  
- **解决**：处理 `LWS_CALLBACK_EVENT_WAIT_CANCELLED`，对有积压的连接 `on_writable`。

### W13. 编不过 / 装不上 ELF

- `LWS_SERVER_OPTION_HTTP_HEADERS_SECURITY_BEST_PRACTICES` 在你这版 lws **不存在** → 删掉，只留 `VALIDATE_UTF8`。  
- 源码目录手写 `Makefile`（`all: metrics_test`）盖住 CMake 链接 → 改名为 `Makefile.host`，包加 `SUPPORTS_IN_SOURCE_BUILD = NO`。

### W14. 板在 Loader 模式，`adb devices` 空

- USB 能看到 `Rockchip rk3xxx 0x2207:0006` 但是 **刷机模式**，不是 adb。  
- **解决**：正常开机进系统后再 `adb devices`。

---

## 六、和代码的对应关系（读 ws_hub 时对照）

| 函数 | 干什么 |
|---|---|
| `ws_hub_start` | 创建 lws、起线程、听 `:8765` |
| `ws_hub_stop` | 退出时收干净 |
| `ws_hub_fanout` | tokq → 所有客户端队列 + 唤醒 lws |
| `ws_hub_status` | UI 右上角 `WS :8765 · n` |
| `callback_llm` | 连接生命周期 + 收发包 |
| `handle_uplink` | 解析 ask → `session_ask` |
| `slot_*` | 每连接有界队列；满则丢旧 token |

`main.c` 里：`tokq_set_fanout(ws_hub_fanout)`，定时器里 `ws_hub_status` → `ui_set_ws_status`。

---

## 七、口诀（可贴显示器）

```text
1. 查 IP：adb shell 'ip -4 addr show wlan0'
2. Mac 与板同一 WiFi
3. 打开 ws_test.html，URL 填 ws://板IP:8765/
4. 屏上要有 WS :8765；push 后记得 chmod 755
5. 1006 → 先查 IP/网络，再查协议版本（≥0.1.2）
```

---

## 八、还没做（别和本文搞混）

- 云端 DeepSeek SSE（PHASE3 步骤 4）  
- TLS / 公网 / frpc  
- BLE GATT  

本文只覆盖：**板子 WiFi + 局域网 WebSocket 流式聊本地模型**。
