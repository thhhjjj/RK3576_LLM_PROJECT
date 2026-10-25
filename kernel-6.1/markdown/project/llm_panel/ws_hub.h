/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LLM_PANEL_WS_HUB_H
#define LLM_PANEL_WS_HUB_H

#include "tokq.h"

/*
 * PHASE3 WebSocket 枢纽（局域网）。
 *
 * 角色：
 *   - 在板子上监听 0.0.0.0:8765，浏览器 / 其它客户端用 ws://板IP:8765/ 连入
 *   - 把 tokq 事件扇出成 JSON 文本帧发给每个连接（与本机屏共用同一条推理流）
 *   - 客户端上行提问 → session_ask（本地 RKLLM）
 *
 * 线程约定（硬规则）：
 *   - RKLLM / tokq 回调线程里禁止 lws_write
 *   - 回调里只：入每连接发送队列 + lws_cancel_service
 *   - 真正发包只在 lws 线程的 LWS_CALLBACK_SERVER_WRITEABLE
 *
 * 帧格式与踩坑见：markdown/project/PHASE/wifi_ws.md
 */

/** 固定监听端口（无 TLS，仅局域网）。 */
#define WS_HUB_PORT 8765

/**
 * 上行提问处理：返回 0 已受理，-1 拒绝（busy / 无 Key 等）。
 * main 注册：本地 session_ask 或云端 cloud_sse_ask。
 */
typedef int (*ws_ask_fn)(const char *user_text);

/** 安装上行 ask 路由；NULL → 回退 session_ask。 */
void ws_hub_set_ask_handler(ws_ask_fn fn);

/**
 * ws_hub_start — 创建 lws 上下文并起服务线程。
 *
 * @return 0 成功（或已启动）；-1 创建失败 / 线程失败
 *
 * 副作用：stderr 打印 "ws_hub: listening 0.0.0.0:8765"
 * 调用时机：main 里 session_load 之后、LVGL 循环之前即可。
 */
int ws_hub_start(void);

/**
 * ws_hub_stop — 停服务线程并销毁 lws 上下文。
 *
 * 进程退出前调用；可重复调用（无上下文时为空操作）。
 */
void ws_hub_stop(void);

/**
 * ws_hub_fanout — tokq 扇出挂钩：把一个事件拷给所有已连接客户端的发送队列。
 *
 * @param ev  tokq 事件（TOKEN / FINISH / ERROR / STATUS）；可为 NULL（忽略）
 *
 * 可在任意线程调用（含 RKLLM 回调）。内部只入队 + cancel_service，不阻塞写 socket。
 * 在 main 里：tokq_set_fanout(ws_hub_fanout);
 */
void ws_hub_fanout(const struct tokq_evt *ev);

/**
 * ws_hub_status — 生成给 UI 用的短状态串。
 *
 * @param buf     输出缓冲
 * @param buflen  缓冲长度（含 '\\0'）
 *
 * 例："WS :8765 · 1" / "WS 未起"
 */
void ws_hub_status(char *buf, unsigned buflen);

#endif
