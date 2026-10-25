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
#define KEY_MAX 256
#define PROMPT_MAX 1500
#define SSE_LINE_MAX 2048

static char g_key[KEY_MAX];
static pthread_mutex_t g_key_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_t g_thr;
static int g_thr_alive;
static pthread_mutex_t g_job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_job_cond = PTHREAD_COND_INITIALIZER;
static int g_have_job;
static int g_stop;
static int g_busy;
static volatile int g_abort;
static char g_pending[PROMPT_MAX];

static CURL *g_curl; /* only touched on worker thread while busy */

struct write_ctx {
	char line[SSE_LINE_MAX];
	size_t len;
	int saw_token;
};

/** Extract delta.content from one SSE data JSON object (best-effort). */
static void emit_delta_content(const char *json)
{
	const char *delta, *p, *s, *e;
	char buf[512];
	size_t n;

	if (!json || !json[0])
		return;
	if (strcmp(json, "[DONE]") == 0)
		return;

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
		return;
	if (*p != '"')
		return;
	s = p + 1;
	/* unescape into buf */
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
				/* skip \uXXXX */
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

static void feed_sse_chunk(struct write_ctx *w, const char *ptr, size_t sz)
{
	size_t i;

	for (i = 0; i < sz; i++) {
		char c = ptr[i];

		if (c == '\r')
			continue;
		if (c == '\n') {
			w->line[w->len] = '\0';
			if (w->len >= 6 && strncmp(w->line, "data:", 5) == 0) {
				const char *payload = w->line + 5;

				while (*payload == ' ')
					payload++;
				if (strcmp(payload, "[DONE]") == 0) {
					/* finish pushed by worker after perform */
				} else if (payload[0]) {
					emit_delta_content(payload);
					w->saw_token = 1;
				}
			}
			w->len = 0;
			continue;
		}
		if (w->len + 1 < sizeof(w->line))
			w->line[w->len++] = c;
	}
}

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	struct write_ctx *w = userdata;
	size_t total = size * nmemb;

	if (g_abort)
		return 0; /* abort transfer */
	feed_sse_chunk(w, ptr, total);
	return total;
}

/* Called between transfers; return non-zero → abort (faster than waiting write). */
static int progress_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
		       curl_off_t ultotal, curl_off_t ulnow)
{
	(void)clientp;
	(void)dltotal;
	(void)dlnow;
	(void)ultotal;
	(void)ulnow;
	return g_abort ? 1 : 0;
}

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

	pthread_mutex_lock(&g_key_lock);
	strncpy(key_copy, g_key, sizeof(key_copy) - 1);
	key_copy[sizeof(key_copy) - 1] = '\0';
	pthread_mutex_unlock(&g_key_lock);

	if (!key_copy[0]) {
		tokq_push(TOKQ_ERROR, "无 API Key");
		return -1;
	}

	/* minimal JSON escape for user text */
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

	curl_easy_setopt(g_curl, CURLOPT_URL, url);
	curl_easy_setopt(g_curl, CURLOPT_POST, 1L);
	curl_easy_setopt(g_curl, CURLOPT_POSTFIELDS, body);
	curl_easy_setopt(g_curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(g_curl, CURLOPT_WRITEDATA, &wctx);
	curl_easy_setopt(g_curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
	curl_easy_setopt(g_curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(g_curl, CURLOPT_TIMEOUT, 120L);
	curl_easy_setopt(g_curl, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(g_curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(g_curl, CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(g_curl, CURLOPT_NOSIGNAL, 1L); /* thread-safe */

	hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
	hdrs = curl_slist_append(hdrs, auth);
	hdrs = curl_slist_append(hdrs, "Accept: text/event-stream");
	curl_easy_setopt(g_curl, CURLOPT_HTTPHEADER, hdrs);

	/* Never log auth / body / key */
	rc = curl_easy_perform(g_curl);
	curl_easy_getinfo(g_curl, CURLINFO_RESPONSE_CODE, &http);

	curl_slist_free_all(hdrs);
	curl_easy_cleanup(g_curl);
	g_curl = NULL;

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

static void *worker(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&g_job_lock);
	while (!g_stop) {
		char prompt[PROMPT_MAX];

		while (!g_have_job && !g_stop)
			pthread_cond_wait(&g_job_cond, &g_job_lock);
		if (g_stop)
			break;
		strncpy(prompt, g_pending, sizeof(prompt) - 1);
		prompt[sizeof(prompt) - 1] = '\0';
		g_have_job = 0;
		g_busy = 1;
		g_abort = 0;
		pthread_mutex_unlock(&g_job_lock);

		run_one(prompt);
		memset(prompt, 0, sizeof(prompt));

		pthread_mutex_lock(&g_job_lock);
		g_busy = 0;
	}
	pthread_mutex_unlock(&g_job_lock);
	return NULL;
}

int cloud_sse_init(void)
{
	curl_global_init(CURL_GLOBAL_DEFAULT);
	g_stop = 0;
	g_have_job = 0;
	g_busy = 0;
	g_abort = 0;
	g_key[0] = '\0';
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

int cloud_sse_has_key(void)
{
	int ok;

	pthread_mutex_lock(&g_key_lock);
	ok = g_key[0] != '\0';
	pthread_mutex_unlock(&g_key_lock);
	return ok;
}

void cloud_sse_load_key_from_env(void)
{
	const char *e;

	if (cloud_sse_has_key())
		return;
	e = getenv("DEEPSEEK_API_KEY");
	if (!e || !e[0])
		e = getenv("LLM_PANEL_API_KEY");
	if (e && e[0])
		cloud_sse_set_key(e);
}

int cloud_sse_ask(const char *user_text)
{
	if (!user_text || !user_text[0])
		return -1;
	if (!cloud_sse_has_key())
		return -1;

	pthread_mutex_lock(&g_job_lock);
	if (g_busy || g_have_job) {
		pthread_mutex_unlock(&g_job_lock);
		return -1;
	}
	strncpy(g_pending, user_text, sizeof(g_pending) - 1);
	g_pending[sizeof(g_pending) - 1] = '\0';
	g_have_job = 1;
	g_abort = 0;
	pthread_cond_signal(&g_job_cond);
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
