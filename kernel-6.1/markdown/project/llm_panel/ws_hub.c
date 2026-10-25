/* SPDX-License-Identifier: GPL-2.0 */
/*
 * PHASE3 WebSocket hub (libwebsockets) — 实现见 ws_hub.h / wifi_ws.md
 *
 * 数据流：
 *   session_ask / RKLLM → tokq_push → ws_hub_fanout → 每连接队列
 *       → lws 线程 WRITEABLE → lws_write(文本 JSON)
 *   浏览器 → RECEIVE → handle_uplink → session_ask
 */

#include "ws_hub.h"

#include "rkllm_session.h"

#include <libwebsockets.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WS_MAX_CLIENTS 4	/* 同时最多几个浏览器/客户端 */
#define WS_Q_CAP 64		/* 每连接发送队列深度 */
#define WS_MSG_MAX 384		/* 单帧 JSON 上限（含结尾 0） */

/** 队列里的一条待发文本帧。 */
struct ws_msg {
	char data[WS_MSG_MAX];
	size_t len;
};

/**
 * 每个 WebSocket 连接一份（lws 按 per_session_data_size 分配）。
 * wsi：本连接句柄，供其它线程入队后 callback_on_writable 用。
 */
struct per_session {
	struct lws *wsi;
	struct ws_msg q[WS_Q_CAP];
	unsigned head, tail, count;
	int active;
};

static struct lws_context *g_ctx;
static pthread_t g_thr;
static volatile int g_run;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_nclients;
static struct per_session *g_slots[WS_MAX_CLIENTS];
static ws_ask_fn g_ask_fn;

void ws_hub_set_ask_handler(ws_ask_fn fn)
{
	g_ask_fn = fn;
}

/**
 * json_escape — 把字符串写成 JSON 字符串内容（转义 \\ \" \\n 等）。
 * @return 写出长度；dst 太小则截断仍保证 NUL 结尾
 */
static int json_escape(char *dst, size_t dstsz, const char *src)
{
	size_t o = 0;
	const unsigned char *p = (const unsigned char *)(src ? src : "");

	if (dstsz < 2)
		return -1;
	while (*p) {
		unsigned char c = *p++;
		const char *esc = NULL;

		if (c == '\\')
			esc = "\\\\";
		else if (c == '"')
			esc = "\\\"";
		else if (c == '\n')
			esc = "\\n";
		else if (c == '\r')
			esc = "\\r";
		else if (c == '\t')
			esc = "\\t";
		else if (c < 0x20)
			continue;

		if (esc) {
			size_t n = strlen(esc);
			if (o + n >= dstsz - 1)
				break;
			memcpy(dst + o, esc, n);
			o += n;
		} else {
			if (o + 1 >= dstsz - 1)
				break;
			dst[o++] = (char)c;
		}
	}
	dst[o] = '\0';
	return (int)o;
}

/**
 * fmt_evt — tokq 事件 → 下行 JSON 文本。
 * FINISH → {"type":"finish"}；其余带 "text" 字段。
 */
static int fmt_evt(char *out, size_t outsz, const struct tokq_evt *ev)
{
	char esc[280];
	const char *type = "status";

	json_escape(esc, sizeof(esc), ev->text);
	switch (ev->kind) {
	case TOKQ_TOKEN:
		type = "token";
		break;
	case TOKQ_FINISH:
		type = "finish";
		break;
	case TOKQ_ERROR:
		type = "error";
		break;
	case TOKQ_STATUS:
		type = "status";
		break;
	}
	if (ev->kind == TOKQ_FINISH)
		return snprintf(out, outsz, "{\"type\":\"finish\"}");
	return snprintf(out, outsz, "{\"type\":\"%s\",\"text\":\"%s\"}", type,
			esc);
}

/**
 * slot_drop_oldest_token — 队列满时优先丢掉最旧的 token 帧，保住 finish/error。
 * 慢客户端隔离：推理不因此停。
 */
static void slot_drop_oldest_token(struct per_session *ps)
{
	while (ps->count > 0) {
		struct ws_msg *m = &ps->q[ps->head];

		ps->head = (ps->head + 1) % WS_Q_CAP;
		ps->count--;
		if (strstr(m->data, "\"type\":\"token\""))
			return;
		if (ps->count + 1 < WS_Q_CAP)
			return;
	}
}

/** slot_enqueue — 往该连接发送队列尾部塞一条消息（满则先 drop）。 */
static int slot_enqueue(struct per_session *ps, const char *msg)
{
	size_t n;

	if (!ps || !msg)
		return -1;
	n = strlen(msg);
	if (n >= WS_MSG_MAX)
		n = WS_MSG_MAX - 1;
	if (ps->count >= WS_Q_CAP)
		slot_drop_oldest_token(ps);
	if (ps->count >= WS_Q_CAP)
		return -1;
	memcpy(ps->q[ps->tail].data, msg, n);
	ps->q[ps->tail].data[n] = '\0';
	ps->q[ps->tail].len = n;
	ps->tail = (ps->tail + 1) % WS_Q_CAP;
	ps->count++;
	return 0;
}

/** slot_dequeue — 取出队头；返回字节数，空队列返回 0。 */
static int slot_dequeue(struct per_session *ps, char *out, size_t outsz)
{
	size_t n;

	if (!ps || ps->count == 0)
		return 0;
	n = ps->q[ps->head].len;
	if (n >= outsz)
		n = outsz - 1;
	memcpy(out, ps->q[ps->head].data, n);
	out[n] = '\0';
	ps->head = (ps->head + 1) % WS_Q_CAP;
	ps->count--;
	return (int)n;
}

/** broadcast_locked — 已持 g_lock：把同一条消息入所有活跃连接队列。 */
static void broadcast_locked(const char *msg)
{
	int i;

	for (i = 0; i < WS_MAX_CLIENTS; i++) {
		if (g_slots[i] && g_slots[i]->active)
			slot_enqueue(g_slots[i], msg);
	}
}

void ws_hub_fanout(const struct tokq_evt *ev)
{
	char msg[WS_MSG_MAX];

	if (!ev || !g_ctx)
		return;
	if (fmt_evt(msg, sizeof(msg), ev) <= 0)
		return;
	pthread_mutex_lock(&g_lock);
	broadcast_locked(msg);
	pthread_mutex_unlock(&g_lock);
	/* 唤醒 lws_service → EVENT_WAIT_CANCELLED → 各连接 on_writable */
	lws_cancel_service(g_ctx);
}

/** send_err_to — 只给某一个连接回错误帧（不广播）。 */
static void send_err_to(struct per_session *ps, const char *text)
{
	char esc[200], msg[WS_MSG_MAX];

	json_escape(esc, sizeof(esc), text);
	snprintf(msg, sizeof(msg), "{\"type\":\"error\",\"text\":\"%s\"}", esc);
	slot_enqueue(ps, msg);
}

/**
 * handle_uplink — 处理客户端发来的文本。
 *
 * 接受：
 *   - 纯文本 = 问题
 *   - {"type":"ask","text":"..."}（简易字符串解析，非完整 JSON 库）
 *
 * 成功：session_ask；并向 tokq 推一条 [WS] 你: … 方便本机屏同步看到提问。
 * 失败：busy / 无模型 / 空问 → 仅回该连接 error 帧。
 */
static void handle_uplink(struct per_session *ps, const char *in, size_t len)
{
	char buf[512];
	const char *q = buf;
	char *p;

	if (!in || len == 0)
		return;
	if (len >= sizeof(buf))
		len = sizeof(buf) - 1;
	memcpy(buf, in, len);
	buf[len] = '\0';

	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
		buf[--len] = '\0';

	p = strstr(buf, "\"type\"");
	if (p && strstr(buf, "ask")) {
		char *t = strstr(buf, "\"text\"");
		char *s, *e;

		if (t) {
			s = strchr(t + 6, '"');
			if (s) {
				s++;
				e = strchr(s, '"');
				if (e) {
					*e = '\0';
					q = s;
				}
			}
		}
	}

	if (!q[0]) {
		send_err_to(ps, "empty ask");
		return;
	}
	{
		struct tokq_evt st;

		st.kind = TOKQ_STATUS;
		snprintf(st.text, sizeof(st.text), "[WS] 你: %.200s", q);
		tokq_push(TOKQ_STATUS, st.text);
	}
	if (g_ask_fn) {
		if (g_ask_fn(q) != 0)
			send_err_to(ps, "busy or unavailable");
	} else if (!session_loaded()) {
		send_err_to(ps, "model not loaded");
	} else if (session_ask(q) != 0) {
		send_err_to(ps, "busy");
	}
}

/**
 * callback_llm — libwebsockets 协议回调（协议名 "llm"）。
 *
 * 关键原因：
 *   ESTABLISHED          新连接：占槽、发 connected
 *   CLOSED               清理槽位（不影响其它连接 / 推理）
 *   RECEIVE              上行提问
 *   SERVER_WRITEABLE     从本连接队列取一条 lws_write
 *   EVENT_WAIT_CANCELLED 其它线程 fanout 后唤醒，给有积压的连接标 writable
 *
 * @return 0 继续；-1 关闭该连接
 */
static int callback_llm(struct lws *wsi, enum lws_callback_reasons reason,
			void *user, void *in, size_t len)
{
	struct per_session *ps = (struct per_session *)user;
	unsigned char out[LWS_PRE + WS_MSG_MAX];
	int n, i;

	switch (reason) {
	case LWS_CALLBACK_ESTABLISHED:
		memset(ps, 0, sizeof(*ps));
		ps->wsi = wsi;
		ps->active = 1;
		pthread_mutex_lock(&g_lock);
		for (i = 0; i < WS_MAX_CLIENTS; i++) {
			if (!g_slots[i]) {
				g_slots[i] = ps;
				g_nclients++;
				break;
			}
		}
		if (i >= WS_MAX_CLIENTS) {
			pthread_mutex_unlock(&g_lock);
			return -1; /* 满员拒绝 */
		}
		pthread_mutex_unlock(&g_lock);
		slot_enqueue(ps, "{\"type\":\"status\",\"text\":\"connected\"}");
		lws_callback_on_writable(wsi);
		break;

	case LWS_CALLBACK_CLOSED:
		pthread_mutex_lock(&g_lock);
		for (i = 0; i < WS_MAX_CLIENTS; i++) {
			if (g_slots[i] == ps) {
				g_slots[i] = NULL;
				if (g_nclients > 0)
					g_nclients--;
				break;
			}
		}
		ps->active = 0;
		ps->wsi = NULL;
		pthread_mutex_unlock(&g_lock);
		break;

	case LWS_CALLBACK_RECEIVE:
		handle_uplink(ps, (const char *)in, len);
		lws_callback_on_writable(wsi);
		break;

	case LWS_CALLBACK_SERVER_WRITEABLE: {
		char msg[WS_MSG_MAX];

		pthread_mutex_lock(&g_lock);
		n = slot_dequeue(ps, msg, sizeof(msg));
		pthread_mutex_unlock(&g_lock);
		if (n > 0) {
			memcpy(&out[LWS_PRE], msg, (size_t)n);
			if (lws_write(wsi, &out[LWS_PRE], (size_t)n,
				      LWS_WRITE_TEXT) < 0)
				return -1;
			pthread_mutex_lock(&g_lock);
			if (ps->count > 0)
				lws_callback_on_writable(wsi);
			pthread_mutex_unlock(&g_lock);
		}
		break;
	}

	case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
		pthread_mutex_lock(&g_lock);
		for (i = 0; i < WS_MAX_CLIENTS; i++) {
			if (g_slots[i] && g_slots[i]->active &&
			    g_slots[i]->wsi && g_slots[i]->count > 0)
				lws_callback_on_writable(g_slots[i]->wsi);
		}
		pthread_mutex_unlock(&g_lock);
		break;

	default:
		return lws_callback_http_dummy(wsi, reason, user, in, len);
	}
	return 0;
}

/*
 * protocols[0] = 默认协议：浏览器常不带 Sec-WebSocket-Protocol，
 * lws 会选列表第一项。名称仍叫 "llm"，试页也可显式 new WebSocket(url, ['llm'])。
 */
static struct lws_protocols protocols[] = {
	{
		.name = "llm",
		.callback = callback_llm,
		.per_session_data_size = sizeof(struct per_session),
		.rx_buffer_size = 1024,
	},
	{ NULL, NULL, 0, 0, 0, NULL, 0 }
};

/** ws_thread — 独立线程里转 lws_service；timeout 50ms 便于响应 cancel。 */
static void *ws_thread(void *arg)
{
	(void)arg;
	while (g_run && g_ctx)
		lws_service(g_ctx, 50);
	return NULL;
}

int ws_hub_start(void)
{
	struct lws_context_creation_info info;

	if (g_ctx)
		return 0;

	memset(&info, 0, sizeof(info));
	info.port = WS_HUB_PORT;
	info.protocols = protocols;
	info.options = LWS_SERVER_OPTION_VALIDATE_UTF8;

	g_ctx = lws_create_context(&info);
	if (!g_ctx) {
		fprintf(stderr, "ws_hub: lws_create_context failed\n");
		return -1;
	}
	g_run = 1;
	if (pthread_create(&g_thr, NULL, ws_thread, NULL) != 0) {
		lws_context_destroy(g_ctx);
		g_ctx = NULL;
		g_run = 0;
		fprintf(stderr, "ws_hub: thread create failed\n");
		return -1;
	}
	fprintf(stderr, "ws_hub: listening 0.0.0.0:%d\n", WS_HUB_PORT);
	return 0;
}

void ws_hub_stop(void)
{
	if (!g_ctx)
		return;
	g_run = 0;
	lws_cancel_service(g_ctx);
	pthread_join(g_thr, NULL);
	lws_context_destroy(g_ctx);
	g_ctx = NULL;
	pthread_mutex_lock(&g_lock);
	memset(g_slots, 0, sizeof(g_slots));
	g_nclients = 0;
	pthread_mutex_unlock(&g_lock);
}

void ws_hub_status(char *buf, unsigned buflen)
{
	int n;

	if (!buf || buflen == 0)
		return;
	pthread_mutex_lock(&g_lock);
	n = g_nclients;
	pthread_mutex_unlock(&g_lock);
	if (!g_ctx)
		snprintf(buf, buflen, "WS 未起");
	else
		snprintf(buf, buflen, "WS :%d · %d", WS_HUB_PORT, n);
}
