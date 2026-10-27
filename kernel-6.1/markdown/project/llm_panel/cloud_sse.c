/* SPDX-License-Identifier: GPL-2.0 */
/*
 * DeepSeek chat/completions stream=true → tokq fan-out.
 * No API key in logs or files.
 */

#include "cloud_sse.h"

#include "tokq.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLOUD_BASE "https://api.deepseek.com"
#define CLOUD_PATH "/chat/completions"
#define CLOUD_MODEL "deepseek-chat"

/* 下列上限是本机静态缓冲，不是 DeepSeek 云端配额；图省堆、可控。 */
#define KEY_MAX 256       /* API Key 长度；固定数组便于 memset 擦除 */
#define PROMPT_MAX 1500   /* 单次提问字符串长度 / POST body；无多轮 hist，超长会截断 */
#define SSE_LINE_MAX 2048 /* 下行单行允许的 SSE 最长字节（拼 data:…）；超长丢后续字符 */

static char g_key[KEY_MAX]; /* 进程内存中的 API Key；不落盘 */
static pthread_mutex_t g_key_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_t g_thr;
static int g_thr_alive;
static pthread_mutex_t g_job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_job_cond = PTHREAD_COND_INITIALIZER;
static int g_have_job; /* 是否有未处理提问（已排队、worker 未取走） */
static int g_stop; /* fini 置 1：唤醒 worker 退出循环，不是单轮 abort */
static int g_busy; /* worker 正在跑 run_one */
static volatile int g_abort; /* 中止本轮：从发出到收到整个动作中止 perform；UI 置 1 */
static char g_pending[PROMPT_MAX]; /* 待处理提问数组：run_one 入参；ask 写入、worker 读取 */


/*
 * 本轮 curl 对象（easy handle）。
 * run_one 里先 setopt 配好（URL/Header/body/回调），再 curl_easy_perform(g_curl) 真正发+收；
 * 仅 worker 在 busy 时碰，结束后 cleanup 置 NULL。
 */
static CURL *g_curl;

/* 用于拼 SSE：write_cb 的 userdata；feed_sse_chunk 跨回调攒行 */
struct write_ctx {
	char line[SSE_LINE_MAX]; /* 当前未凑齐的一行 */
	size_t len;              /* line 已有字节数 */
	int saw_token;           /* 本轮是否已推出过 TOKEN（结束时区分「无内容」） */
};

/*
 * 从一行 SSE data JSON 里抠 delta.content → tokq TOKEN。
 * 手写 best-effort，非完整 JSON 解析；本段 content 进 tokq 上限约 512 字节。
 */
static void emit_delta_content(const char *json)
{
	const char *delta, *p, *s, *e;
	char buf[512];
	size_t n;

	if (!json || !json[0])
		return;
	/* 流结束标记由 worker 在 perform 后推 FINISH，这里直接丢 */
	if (strcmp(json, "[DONE]") == 0)
		return;

	/* 优先 delta.content；没有 delta 再退回整段 content */
	delta = strstr(json, "\"delta\"");
	p = delta ? strstr(delta, "\"content\"") : strstr(json, "\"content\"");
	if (!p)
		return;
	p = strchr(p + 8, ':');
	if (!p)
		return;
	p++;
	while (*p == ' ' || *p == '\t')
		p++;
	if (*p == 'n' && strncmp(p, "null", 4) == 0)
		return; /* role 等字段常带 "content": null */
	if (*p != '"')
		return;
	s = p + 1;

	/* JSON 字符串反转义 → buf，再推 tokq */
	n = 0;
	for (e = s; *e && *e != '"' && n + 1 < sizeof(buf); e++) {
		if (*e == '\\' && e[1]) {
			e++;
			if (*e == 'n')
				buf[n++] = '\n';
			else if (*e == 't')
				buf[n++] = '\t';
			else if (*e == '"' || *e == '\\' || *e == '/')
				buf[n++] = *e;
			else if (*e == 'u') {
				/* \uXXXX 暂不解码，跳过四位 hex */
				int k;
				for (k = 0; k < 4 && e[1]; k++)
					e++;
			} else
				buf[n++] = *e;
		} else
			buf[n++] = *e;
	}
	buf[n] = '\0';
	if (n > 0)
		tokq_push(TOKQ_TOKEN, buf);
}

/*
 * 按 SSE 行解析下行数据：curl 字节流先拼行，data: 行再交给 emit_delta_content。
 * 行缓冲上限 SSE_LINE_MAX；超长丢后续字符；非 data: 行忽略。
 */
static void feed_sse_chunk(struct write_ctx *w, const char *ptr, size_t sz)
{
	size_t i;

	for (i = 0; i < sz; i++) {
		char c = ptr[i];

		if (c == '\r')
			continue; /* 统一吃 \n 作为行尾 */
		if (c == '\n') {
			w->line[w->len] = '\0';
			if (w->len >= 6 && strncmp(w->line, "data:", 5) == 0) {
				const char *payload = w->line + 5;

				while (*payload == ' ')
					payload++;
				if (strcmp(payload, "[DONE]") == 0) {
					/* FINISH 留给 worker 在 perform 结束后推 */
				} else if (payload[0]) {
					emit_delta_content(payload);
					w->saw_token = 1; /* 本轮至少出过字 */
				}
			}
			w->len = 0; /* 开始下一行 */
			continue;
		}
		/* 未满则追加；满了丢后续字节直到换行 */
		if (w->len + 1 < sizeof(w->line))
			w->line[w->len++] = c;
	}
}

/*
 * libcurl CURLOPT_WRITEFUNCTION：下行数据回调（名字叫 write，实为「收包」）。
 * 把本段字节交给 feed_sse_chunk；须返回已消费字节数，返回 0 则中止传输。
 */
static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	struct write_ctx *w = userdata;
	size_t total = size * nmemb;

	if (g_abort)
		return 0; /* 配合 cloud_sse_abort：中止 perform */
	feed_sse_chunk(w, ptr, total);
	return total;
}

/*
 * libcurl 进度回调（CURLOPT_XFERINFOFUNCTION）：传输过程中周期性调用。
 * 不看进度数字，只盯 g_abort；返回非 0 立刻中止 perform。
 * 比干等下一次 write_cb 更快响应「停止」（卡在无下行数据时也能掐）。
 */
static int progress_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
		       curl_off_t ultotal, curl_off_t ulnow)
{
	(void)clientp;
	(void)dltotal;
	(void)dlnow;
	(void)ultotal;
	(void)ulnow;
	/* 1 = 中止 curl；0 = 继续。不看进度，只响应 cloud_sse_abort */
	return g_abort ? 1 : 0;
}

/*
 * 跑完一轮云端问答（仅 worker 调用）：拼 POST → setopt 配 g_curl → perform 发+收。
 * 下行经 write_cb→feed_sse→emit 推 tokq；结束推 FINISH/ERROR。成功 0 / 失败 -1。
 */
static int run_one(const char *user_text)
{
	char url[128];
	char auth[KEY_MAX + 32];
	char body[PROMPT_MAX + 256];
	char esc[PROMPT_MAX];
	struct curl_slist *hdrs = NULL;
	struct write_ctx wctx;
	CURLcode rc;
	long http = 0;
	size_t i, o;
	char key_copy[KEY_MAX];

	/* 短锁拷 Key：perform 期间不占 g_key_lock，UI 仍可 set_key；本轮用拷贝，新 Key 下轮生效 */
	pthread_mutex_lock(&g_key_lock);
	strncpy(key_copy, g_key, sizeof(key_copy) - 1);
	key_copy[sizeof(key_copy) - 1] = '\0';
	pthread_mutex_unlock(&g_key_lock);

	/* 无 Key 直接退出 */
	if (!key_copy[0]) {
		tokq_push(TOKQ_ERROR, "无 API Key");
		return -1;
	}

	/* 提问做最小 JSON 转义，再塞进 messages[].content */
	o = 0;
	for (i = 0; user_text[i] && o + 2 < sizeof(esc); i++) {
		char c = user_text[i];

		if (c == '\\' || c == '"') {
			esc[o++] = '\\';
			esc[o++] = c;
		} else if (c == '\n') {
			esc[o++] = '\\';
			esc[o++] = 'n';
		} else if ((unsigned char)c >= 0x20)
			esc[o++] = c;
	}
	esc[o] = '\0';

	/* 拼 URL/Auth/Body */
	snprintf(url, sizeof(url), "%s%s", CLOUD_BASE, CLOUD_PATH);
	snprintf(auth, sizeof(auth), "Authorization: Bearer %s", key_copy);
	snprintf(body, sizeof(body),
		 "{\"model\":\"%s\",\"stream\":true,\"messages\":"
		 "[{\"role\":\"user\",\"content\":\"%s\"}]}",
		 CLOUD_MODEL, esc);

	memset(&wctx, 0, sizeof(wctx));
	g_curl = curl_easy_init();
	if (!g_curl) {
		tokq_push(TOKQ_ERROR, "curl init 失败");
		return -1;
	}

	/* 拼/配置 g_curl（尚未上网）；真正发+收在下面 perform */
	curl_easy_setopt(g_curl, CURLOPT_URL, url);
	curl_easy_setopt(g_curl, CURLOPT_POST, 1L);
	curl_easy_setopt(g_curl, CURLOPT_POSTFIELDS, body);
	curl_easy_setopt(g_curl, CURLOPT_WRITEFUNCTION, write_cb); /* 下行收包 */
	curl_easy_setopt(g_curl, CURLOPT_WRITEDATA, &wctx);
	curl_easy_setopt(g_curl, CURLOPT_XFERINFOFUNCTION, progress_cb); /* 中止钩子 */
	curl_easy_setopt(g_curl, CURLOPT_NOPROGRESS, 0L); /* 0=启用进度回调 */
	curl_easy_setopt(g_curl, CURLOPT_TIMEOUT, 120L);
	curl_easy_setopt(g_curl, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(g_curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(g_curl, CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(g_curl, CURLOPT_NOSIGNAL, 1L); /* 多线程下更安全 */

	/* HTTP 请求头：JSON body + Bearer Key + 声明要 SSE 流 */
	hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
	hdrs = curl_slist_append(hdrs, auth); /* Authorization: Bearer … */
	hdrs = curl_slist_append(hdrs, "Accept: text/event-stream");
	
	curl_easy_setopt(g_curl, CURLOPT_HTTPHEADER, hdrs);

	/* Never log auth / body / key */
	/* 真正发+收（阻塞至结束/中止）；下行在 write_cb 里已推 tokq */
	rc = curl_easy_perform(g_curl);
	curl_easy_getinfo(g_curl, CURLINFO_RESPONSE_CODE, &http); /* HTTP 状态码 */

	curl_slist_free_all(hdrs); /* 释放请求头链表 */
	curl_easy_cleanup(g_curl); /* 销毁本轮 curl 对象 */
	g_curl = NULL;

	/* 擦本轮缓冲：清空拷贝 Key / 临时 Auth/Body/esc；仅 g_curl 指针清零 */
	memset(key_copy, 0, sizeof(key_copy));
	memset(auth, 0, sizeof(auth));
	memset(body, 0, sizeof(body));
	memset(esc, 0, sizeof(esc));

	if (g_abort) {
		tokq_push(TOKQ_STATUS, "[云端已中止]");
		tokq_push(TOKQ_FINISH, NULL);
		return 0;
	}
	if (rc == CURLE_OPERATION_TIMEDOUT) {
		tokq_push(TOKQ_ERROR, "云端超时");
		return -1;
	}
	if (rc == CURLE_WRITE_ERROR && g_abort) {
		tokq_push(TOKQ_FINISH, NULL);
		return 0;
	}
	if (rc != CURLE_OK) {
		char err[96];

		snprintf(err, sizeof(err), "云端断流(%d)", (int)rc);
		tokq_push(TOKQ_ERROR, err);
		return -1;
	}
	if (http < 200 || http >= 300) {
		char err[96];

		snprintf(err, sizeof(err), "HTTP %ld", http);
		tokq_push(TOKQ_ERROR, err);
		return -1;
	}
	if (!wctx.saw_token)
		tokq_push(TOKQ_STATUS, "[云端无内容]");
	tokq_push(TOKQ_FINISH, NULL);
	return 0;
}

/*
 * 云端 worker 线程：等 ask 排队 → 取 prompt → run_one（一整轮发+收）→ 再等。
 * 外圈是「多轮问答」；单轮 SSE 收包循环在 curl perform 内部，不在这里。
 */
static void *worker(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&g_job_lock);
	while (!g_stop) {
		char prompt[PROMPT_MAX];

		/* 无活就睡；ask 里 signal 后醒来（仍持 g_job_lock） */
		while (!g_have_job && !g_stop)
			pthread_cond_wait(&g_job_cond, &g_job_lock);
		if (g_stop)
			break; /* fini：退线程 */

		/* 取走排队提问，标记 busy；解锁后再跑，避免 perform 长时间占锁 */
		strncpy(prompt, g_pending, sizeof(prompt) - 1);
		prompt[sizeof(prompt) - 1] = '\0';
		g_have_job = 0;
		g_busy = 1;
		g_abort = 0;
		pthread_mutex_unlock(&g_job_lock);

		run_one(prompt); /* 阻塞至本轮结束/中止；出字经 write_cb→tokq */
		memset(prompt, 0, sizeof(prompt));

		pthread_mutex_lock(&g_job_lock);
		g_busy = 0; /* 可接下一次 ask */
	}
	pthread_mutex_unlock(&g_job_lock);
	return NULL;
}

/* 初始化云端 SSE：curl_global_init + 拉起 worker 线程（本函数不是 worker）。失败 -1。 */
int cloud_sse_init(void)
{
	/* 进程级 curl 初始化（SSL 等），配对 fini 里的 curl_global_cleanup */
	curl_global_init(CURL_GLOBAL_DEFAULT);

	/* 复位任务状态；Key 空着，等 set_key / load_key_from_env */
	g_stop = 0;
	g_have_job = 0;
	g_busy = 0;
	g_abort = 0;
	g_key[0] = '\0';

	/* 后台 worker：cond 等 job，跑 run_one → tokq */
	if (pthread_create(&g_thr, NULL, worker, NULL) != 0) {
		g_thr_alive = 0;
		return -1;
	}
	g_thr_alive = 1;
	return 0;
}

void cloud_sse_fini(void)
{
	cloud_sse_abort();
	pthread_mutex_lock(&g_job_lock);
	g_stop = 1;
	pthread_cond_signal(&g_job_cond);
	pthread_mutex_unlock(&g_job_lock);
	if (g_thr_alive) {
		pthread_join(g_thr, NULL);
		g_thr_alive = 0;
	}
	pthread_mutex_lock(&g_key_lock);
	memset(g_key, 0, sizeof(g_key));
	pthread_mutex_unlock(&g_key_lock);
	curl_global_cleanup();
}

void cloud_sse_set_key(const char *key)
{
	pthread_mutex_lock(&g_key_lock);
	if (!key || !key[0])
		g_key[0] = '\0';
	else {
		strncpy(g_key, key, sizeof(g_key) - 1);
		g_key[sizeof(g_key) - 1] = '\0';
	}
	pthread_mutex_unlock(&g_key_lock);
}

/* 进程内存里是否已有 API Key；1 有 / 0 无。不读盘、不回传 Key 内容。 */
int cloud_sse_has_key(void)
{
	int ok;

	/* 与 set_key 同锁，避免并发读写 g_key */
	pthread_mutex_lock(&g_key_lock);
	ok = g_key[0] != '\0';
	pthread_mutex_unlock(&g_key_lock);
	return ok;
}

/* 启动期从环境变量灌 Key 到进程内存；已有 Key 不覆盖。不落盘、不打日志。 */
void cloud_sse_load_key_from_env(void)
{
	const char *e;

	/* UI / 先前 set_key 已写过则跳过 */
	if (cloud_sse_has_key())
		return;

	/* 优先 DEEPSEEK_API_KEY，其次 LLM_PANEL_API_KEY */
	e = getenv("DEEPSEEK_API_KEY");
	if (!e || !e[0])
		e = getenv("LLM_PANEL_API_KEY");
	if (e && e[0])
		cloud_sse_set_key(e);
}

/*
 * 排队一次云端提问（UI/WS 调用，很快返回）。
 * 写入 g_pending、置 g_have_job，signal 叫醒 worker；真正发收在 worker→run_one。
 * 0 已排队；-1 空串/无 Key/忙（busy 或已有未取 job）。
 */
int cloud_sse_ask(const char *user_text)
{
	if (!user_text || !user_text[0])
		return -1;
	if (!cloud_sse_has_key())
		return -1;

	pthread_mutex_lock(&g_job_lock);
	if (g_busy || g_have_job) {
		pthread_mutex_unlock(&g_job_lock);
		return -1; /* 单 worker：忙则拒，不堆第二问 */
	}
	strncpy(g_pending, user_text, sizeof(g_pending) - 1);
	g_pending[sizeof(g_pending) - 1] = '\0';
	g_have_job = 1; /* worker 内圈 while 醒来的条件 */
	g_abort = 0;
	pthread_cond_signal(&g_job_cond); /* 叫醒 cond_wait 中的 worker */
	pthread_mutex_unlock(&g_job_lock);
	return 0;
}

void cloud_sse_abort(void)
{
	g_abort = 1;
	/* write_cb returning 0 aborts perform; no easy handle store for pause */
}

int cloud_sse_busy(void)
{
	int b;

	pthread_mutex_lock(&g_job_lock);
	b = g_busy || g_have_job;
	pthread_mutex_unlock(&g_job_lock);
	return b;
}
