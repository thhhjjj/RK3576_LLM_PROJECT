/* SPDX-License-Identifier: MIT */
#ifndef LLM_PANEL_FRPC_CTL_H
#define LLM_PANEL_FRPC_CTL_H

/*
 * PHASE5：frpc 起停（默认关，不进开机脚本）。
 * 配置：/etc/frpc.toml（TCP 转本机 WS_HUB_PORT）
 * 二进制：/usr/bin/frpc（可 adb 推送）
 */

#define FRPC_BIN "/usr/bin/frpc"
#define FRPC_CONF "/etc/frpc.toml"
#define FRPC_PIDFILE "/run/frpc.pid"
#define FRPC_LOG "/tmp/frpc.log"

/* 0=关 1=运行中 -1=配置/二进制缺失 */
int frpc_ctl_running(void);

/* 启动；成功 0，失败 -1（缺文件/已在跑/exec 失败） */
int frpc_ctl_start(void);

/* 停止；成功 0 */
int frpc_ctl_stop(void);

/* 写入 status 缓冲，如 "frpc 关" / "frpc 开" / "frpc 缺配置" */
void frpc_ctl_status(char *buf, int buflen);

#endif
