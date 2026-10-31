# 板端 frpc 安装（PHASE5）

本机下载环境若拦 GitHub，在 Mac 上执行：

```bash
VER=0.61.1
curl -fsSL -o /tmp/frp.tgz \
  "https://github.com/fatedier/frp/releases/download/v${VER}/frp_${VER}_linux_arm64.tar.gz"
tar -xzf /tmp/frp.tgz -C /tmp
adb push /tmp/frp_${VER}_linux_arm64/frpc /usr/bin/frpc
adb shell 'chmod 755 /usr/bin/frpc; frpc --version'

adb push llm_panel/rootfs_wifi/etc/frpc.toml.example /etc/frpc.toml.example
adb shell 'cp /etc/frpc.toml.example /etc/frpc.toml; chmod 600 /etc/frpc.toml'
# 编辑 serverAddr / auth.token / remotePort
adb shell 'vi /etc/frpc.toml'
```

服务端 `frps.toml` 最小例：

```toml
bindPort = 7000
auth.method = "token"
auth.token = "与板子相同"
```

公网试连：`ws://公网IP:remotePort/`（默认模板 remotePort=18765）。
