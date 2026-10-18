/* SPDX-License-Identifier: GPL-2.0 */
/*
 * PHASE2: dlopen librkllmrt, history + compress, worker thread for rkllm_run.
 * Callback only pushes to tokq — never touches LVGL.
 */

#include "rkllm_session.h"

#include "tokq.h"
#include "vendor/rkllm.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define HIST_MAX 32
#define MSG_MAX 4096
#define PROMPT_MAX (256 * 1024)

static const char *SYSTEM_PROMPT =
	"你是嵌入式 Linux 助手，擅长 C、设备树、RK 驱动；回答务必简洁。"
	"只输出最终答案，不要输出任何推理过程、草稿或分点分析。";

static const char *SMOKE_Q = "用一句话介绍 RK3576 上的 RKNPU。";

/* Min kept answer bytes before we treat trailing "Thinking Process" as junk. */
#define THINK_ANSWER_MIN 12

struct hist_msg {
	char role; /* 's' system, 'u' user, 'a' assistant */
	char text[MSG_MAX];
};

typedef RKLLMParam (*fn_createDefaultParam)(void);
typedef int (*fn_init)(LLMHandle *, RKLLMParam *, RKLLMCallback *);
typedef int (*fn_destroy)(LLMHandle);
typedef int (*fn_run)(LLMHandle, RKLLMInput *, RKLLMInferParam *, void *);
typedef int (*fn_abort)(LLMHandle);
typedef int (*fn_is_running)(LLMHandle);
typedef int (*fn_get_kv)(LLMHandle, int *);
typedef int (*fn_set_chat_template)(LLMHandle, const char *, const char *,
				   const char *);

static void *lib;
static fn_createDefaultParam p_createDefaultParam;
static fn_init p_init;
static fn_destroy p_destroy;
static fn_run p_run;
static fn_abort p_abort;
static fn_is_running p_is_running;
static fn_get_kv p_get_kv;
static fn_set_chat_template p_set_chat_template;

static LLMHandle handle;
static int loaded;
static int compressing;
static char model_path_saved[512];

static struct hist_msg hist[HIST_MAX];
static int hist_n;

static pthread_t worker;
static pthread_mutex_t wlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wcond = PTHREAD_COND_INITIALIZER;
static int worker_live;
static int worker_stop;
static int have_job;
static char pending_user[MSG_MAX];

static int gen_tokens;
static struct timespec gen_t0;
static float last_tps;
static int last_ctx = -1;

static char reply_acc[PROMPT_MAX];
static size_t reply_acc_n;
static pthread_mutex_t reply_lock = PTHREAD_MUTEX_INITIALIZER;
/*
 * drop_thinking: answer already shown; ignore rest (and abort).
 * skip_pre_think: model started with Thinking Process; wait for CJK answer.
 */
static int drop_thinking;
static int skip_pre_think;
/* Compress rebuild: feed retained hist into new handle without showing tokens. */
static int prefilling;

static void hist_reset_keep_system(void)
{
	hist_n = 0;
	hist[0].role = 's';
	strncpy(hist[0].text, SYSTEM_PROMPT, MSG_MAX - 1);
	hist[0].text[MSG_MAX - 1] = '\0';
	hist_n = 1;
}

static void hist_push(char role, const char *text)
{
	if (hist_n >= HIST_MAX) {
		int i;

		if (hist_n <= 1)
			return;
		for (i = 1; i < hist_n - 1; i++)
			hist[i] = hist[i + 1];
		hist_n--;
	}
	hist[hist_n].role = role;
	strncpy(hist[hist_n].text, text ? text : "", MSG_MAX - 1);
	hist[hist_n].text[MSG_MAX - 1] = '\0';
	hist_n++;
}

static void refresh_ctx(void)
{
	int sizes[8];

	last_ctx = -1;
	if (!handle || !p_get_kv)
		return;
	memset(sizes, 0, sizeof(sizes));
	if (p_get_kv(handle, sizes) == 0)
		last_ctx = sizes[0];
}

static char *find_thinking_mark(char *s)
{
	static const char *marks[] = {
		"Thinking Process",
		"thinking process",
		"<think>",
		"\n思维过程",
		"\n思维链",
		NULL,
	};
	char *best = NULL;
	int i;

	for (i = 0; marks[i]; i++) {
		char *p = strstr(s, marks[i]);

		if (p && (!best || p < best))
			best = p;
	}
	return best;
}

static const char *first_cjk_utf8(const char *s)
{
	const unsigned char *p = (const unsigned char *)s;

	while (*p) {
		/* Rough CJK Unified Ideographs UTF-8 lead bytes. */
		if (p[0] >= 0xE4 && p[0] <= 0xE9 && p[1] && p[2])
			return (const char *)p;
		p++;
	}
	return NULL;
}

static void trim_thinking_tail(void)
{
	char *mark = find_thinking_mark(reply_acc);
	size_t keep;

	if (!mark)
		return;
	keep = (size_t)(mark - reply_acc);
	while (keep > 0 &&
	       (reply_acc[keep - 1] == '\n' || reply_acc[keep - 1] == '\r' ||
		reply_acc[keep - 1] == ' '))
		keep--;
	reply_acc[keep] = '\0';
	reply_acc_n = keep;
}

static int result_cb(RKLLMResult *result, void *userdata, LLMCallState state)
{
	(void)userdata;

	if (state == RKLLM_RUN_ERROR) {
		if (prefilling)
			return 0;
		pthread_mutex_lock(&reply_lock);
		reply_acc_n = 0;
		reply_acc[0] = '\0';
		drop_thinking = 0;
		skip_pre_think = 0;
		pthread_mutex_unlock(&reply_lock);
		tokq_push(TOKQ_ERROR, "推理错误");
		return 0;
	}

	if (state == RKLLM_RUN_NORMAL && result && result->text &&
	    result->text[0]) {
		size_t len = strlen(result->text);
		size_t old_n;
		char *mark;
		char partial[512];
		const char *cjk;

		/* Prefill after compress: stop ASAP, never paint UI. */
		if (prefilling) {
			if (handle && p_abort)
				p_abort(handle);
			return 0;
		}

		pthread_mutex_lock(&reply_lock);
		if (drop_thinking) {
			pthread_mutex_unlock(&reply_lock);
			return 0;
		}

		/* Thinking came first: wait until CJK answer tokens appear. */
		if (skip_pre_think) {
			cjk = first_cjk_utf8(result->text);
			if (!cjk) {
				pthread_mutex_unlock(&reply_lock);
				return 0;
			}
			skip_pre_think = 0;
			len = strlen(cjk);
			gen_tokens++;
			if (reply_acc_n + len < sizeof(reply_acc) - 1) {
				memcpy(reply_acc + reply_acc_n, cjk, len);
				reply_acc_n += len;
				reply_acc[reply_acc_n] = '\0';
			}
			pthread_mutex_unlock(&reply_lock);
			tokq_push(TOKQ_TOKEN, cjk);
			return 0;
		}

		gen_tokens++;
		old_n = reply_acc_n;
		if (reply_acc_n + len < sizeof(reply_acc) - 1) {
			memcpy(reply_acc + reply_acc_n, result->text, len);
			reply_acc_n += len;
			reply_acc[reply_acc_n] = '\0';
		}

		mark = find_thinking_mark(reply_acc);
		if (mark) {
			size_t keep = (size_t)(mark - reply_acc);

			while (keep > 0 &&
			       (reply_acc[keep - 1] == '\n' ||
				reply_acc[keep - 1] == '\r' ||
				reply_acc[keep - 1] == ' '))
				keep--;

			if (keep < THINK_ANSWER_MIN) {
				/* Think-then-answer: discard prefix, wait for CJK. */
				reply_acc_n = 0;
				reply_acc[0] = '\0';
				skip_pre_think = 1;
				pthread_mutex_unlock(&reply_lock);
				fprintf(stderr,
					"rkllm_session: skip leading think\n");
				return 0;
			}

			/* Answer-then-think: keep answer, drop the rest. */
			reply_acc[keep] = '\0';
			reply_acc_n = keep;
			drop_thinking = 1;
			{
				size_t show = (keep > old_n) ? (keep - old_n) : 0;

				if (show > 0 && show < sizeof(partial)) {
					memcpy(partial, result->text, show);
					partial[show] = '\0';
				} else {
					partial[0] = '\0';
				}
			}
			pthread_mutex_unlock(&reply_lock);

			if (partial[0])
				tokq_push(TOKQ_TOKEN, partial);
			fprintf(stderr,
				"rkllm_session: cut trailing think, abort\n");
			if (handle && p_abort)
				p_abort(handle);
			return 0;
		}
		pthread_mutex_unlock(&reply_lock);
		tokq_push(TOKQ_TOKEN, result->text);
		return 0;
	}

	if (state == RKLLM_RUN_FINISH) {
		struct timespec t1;
		double sec;

		if (prefilling)
			return 0;

		clock_gettime(CLOCK_MONOTONIC, &t1);
		sec = (t1.tv_sec - gen_t0.tv_sec) +
		      (t1.tv_nsec - gen_t0.tv_nsec) / 1e9;
		if (sec > 0.05 && gen_tokens > 0)
			last_tps = (float)gen_tokens / (float)sec;

		{
			int empty;

			pthread_mutex_lock(&reply_lock);
			trim_thinking_tail();
			empty = (reply_acc_n == 0);
			if (!empty)
				hist_push('a', reply_acc);
			reply_acc_n = 0;
			reply_acc[0] = '\0';
			drop_thinking = 0;
			skip_pre_think = 0;
			pthread_mutex_unlock(&reply_lock);

			if (empty)
				tokq_push(TOKQ_STATUS, "[无有效回答]");
			tokq_push(TOKQ_FINISH, NULL);
		}
		return 0;
	}
	return 0;
}

static int resolve_syms(void)
{
	p_createDefaultParam = (fn_createDefaultParam)dlsym(
		lib, "rkllm_createDefaultParam");
	p_init = (fn_init)dlsym(lib, "rkllm_init");
	p_destroy = (fn_destroy)dlsym(lib, "rkllm_destroy");
	p_run = (fn_run)dlsym(lib, "rkllm_run");
	p_abort = (fn_abort)dlsym(lib, "rkllm_abort");
	p_is_running = (fn_is_running)dlsym(lib, "rkllm_is_running");
	p_get_kv = (fn_get_kv)dlsym(lib, "rkllm_get_kv_cache_size");
	p_set_chat_template =
		(fn_set_chat_template)dlsym(lib, "rkllm_set_chat_template");

	if (!p_createDefaultParam || !p_init || !p_destroy || !p_run ||
	    !p_abort) {
		fprintf(stderr, "rkllm_session: missing symbols: %s\n",
			dlerror());
		return -1;
	}
	return 0;
}

static int open_lib(void)
{
	const char *path = getenv("LLM_PANEL_LIB");

	if (!path || !path[0])
		path = LLM_PANEL_DEFAULT_LIB;
	lib = dlopen(path, RTLD_NOW);
	if (!lib) {
		fprintf(stderr, "rkllm_session: dlopen %s failed: %s\n", path,
			dlerror());
		return -1;
	}
	return resolve_syms();
}

static void destroy_handle(void)
{
	if (handle && p_destroy) {
		p_abort(handle);
		p_destroy(handle);
	}
	handle = NULL;
	loaded = 0;
}

static int create_handle(const char *model_path)
{
	RKLLMParam param;
	RKLLMCallback cb;
	int ret;

	param = p_createDefaultParam();
	param.model_path = model_path;
	param.max_context_len = LLM_PANEL_MAX_CONTEXT;
	param.max_new_tokens = 512;
	param.top_k = 1;
	param.top_p = 0.9f;
	param.temperature = 0.8f;
	param.repeat_penalty = 1.1f;
	param.skip_special_token = true;
	param.is_async = false;

	memset(&cb, 0, sizeof(cb));
	cb.result_callback = result_cb;
	cb.result_userdata = NULL;

	handle = NULL;
	ret = p_init(&handle, &param, &cb);
	if (ret != 0 || !handle) {
		fprintf(stderr, "rkllm_session: rkllm_init failed (%d)\n", ret);
		handle = NULL;
		return -1;
	}
	/*
	 * Keep a short system prompt. Note: set_chat_template disables the
	 * runtime's automatic enable_thinking handling — we still pass
	 * enable_thinking=false and append /no_think on every user turn.
	 */
	if (p_set_chat_template)
		p_set_chat_template(handle, SYSTEM_PROMPT, "", "");
	return 0;
}

static void *worker_main(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&wlock);
	while (!worker_stop) {
		char user[MSG_MAX];
		RKLLMInput input;
		RKLLMInferParam infer;
		int ret;

		while (!have_job && !worker_stop)
			pthread_cond_wait(&wcond, &wlock);
		if (worker_stop)
			break;

		strncpy(user, pending_user, sizeof(user) - 1);
		user[sizeof(user) - 1] = '\0';
		have_job = 0;
		pthread_mutex_unlock(&wlock);

		if (!loaded || !handle) {
			tokq_push(TOKQ_ERROR, "模型未加载");
			pthread_mutex_lock(&wlock);
			continue;
		}

		hist_push('u', user);

		pthread_mutex_lock(&reply_lock);
		reply_acc_n = 0;
		reply_acc[0] = '\0';
		drop_thinking = 0;
		skip_pre_think = 0;
		pthread_mutex_unlock(&reply_lock);

		gen_tokens = 0;
		clock_gettime(CLOCK_MONOTONIC, &gen_t0);

		/*
		 * Qwen3.5: thinking burns tokens + latency. Flag alone is not
		 * enough after set_chat_template; /no_think is the reliable off.
		 */
		{
			static char prompt_buf[MSG_MAX + 32];

			snprintf(prompt_buf, sizeof(prompt_buf), "%s\n/no_think",
				 user);
			memset(&input, 0, sizeof(input));
			input.role = "user";
			input.enable_thinking = false;
			input.input_type = RKLLM_INPUT_PROMPT;
			input.prompt_input = prompt_buf;

			memset(&infer, 0, sizeof(infer));
			infer.mode = RKLLM_INFER_GENERATE;
			infer.keep_history = 1;

			ret = p_run(handle, &input, &infer, NULL);
		}
		if (ret != 0)
			tokq_push(TOKQ_ERROR, "rkllm_run 失败");

		refresh_ctx();
		if (last_ctx > 0 &&
		    last_ctx * 100 >=
			    LLM_PANEL_MAX_CONTEXT * LLM_PANEL_COMPRESS_WATER_PCT) {
			tokq_push(TOKQ_STATUS, "[自动压缩]");
			session_compress();
		}

		pthread_mutex_lock(&wlock);
	}
	pthread_mutex_unlock(&wlock);
	return NULL;
}

int session_init(void)
{
	hist_reset_keep_system();
	tokq_init();
	worker_stop = 0;
	have_job = 0;
	worker_live = 0;
	return 0;
}

void session_fini(void)
{
	session_abort();
	pthread_mutex_lock(&wlock);
	worker_stop = 1;
	pthread_cond_signal(&wcond);
	pthread_mutex_unlock(&wlock);
	if (worker_live) {
		pthread_join(worker, NULL);
		worker_live = 0;
	}
	destroy_handle();
	if (lib) {
		dlclose(lib);
		lib = NULL;
	}
}

int session_load(const char *model_path)
{
	const char *path = model_path;

	if (!path || !path[0])
		path = getenv("LLM_PANEL_MODEL");
	if (!path || !path[0])
		path = LLM_PANEL_DEFAULT_MODEL;

	strncpy(model_path_saved, path, sizeof(model_path_saved) - 1);
	model_path_saved[sizeof(model_path_saved) - 1] = '\0';

	if (!lib && open_lib() != 0)
		return -1;

	destroy_handle();
	hist_reset_keep_system();

	if (create_handle(path) != 0)
		return -1;

	loaded = 1;
	tokq_push(TOKQ_STATUS, "[模型已加载]");

	if (!worker_live) {
		worker_stop = 0;
		if (pthread_create(&worker, NULL, worker_main, NULL) != 0) {
			fprintf(stderr, "rkllm_session: worker create failed\n");
			destroy_handle();
			return -1;
		}
		worker_live = 1;
	}
	fprintf(stderr, "rkllm_session: loaded %s\n", path);
	return 0;
}

int session_loaded(void)
{
	return loaded;
}

int session_ask(const char *user_text)
{
	if (!user_text || !user_text[0] || !loaded)
		return -1;

	pthread_mutex_lock(&wlock);
	/* Prior abort can leave is_running sticky — clear then accept. */
	if (handle && p_is_running && p_is_running(handle)) {
		if (p_abort)
			p_abort(handle);
		pthread_mutex_unlock(&wlock);
		usleep(50000);
		pthread_mutex_lock(&wlock);
	}
	if (have_job || (handle && p_is_running && p_is_running(handle))) {
		pthread_mutex_unlock(&wlock);
		return -1;
	}
	strncpy(pending_user, user_text, sizeof(pending_user) - 1);
	pending_user[sizeof(pending_user) - 1] = '\0';
	have_job = 1;
	pthread_cond_signal(&wcond);
	pthread_mutex_unlock(&wlock);
	return 0;
}

void session_abort(void)
{
	if (handle && p_abort)
		p_abort(handle);
}

/* After destroy+create, KV is empty — re-feed retained hist (system already in template). */
static int prefill_retained_hist(void)
{
	static char buf[PROMPT_MAX];
	size_t n = 0;
	int i;
	RKLLMInput input;
	RKLLMInferParam infer;
	int ret;

	if (!handle || hist_n <= 1)
		return 0;

	buf[0] = '\0';
	for (i = 1; i < hist_n; i++) {
		const char *tag = (hist[i].role == 'u') ? "用户: " : "助手: ";
		size_t tl = strlen(tag);
		size_t hl = strlen(hist[i].text);

		if (n + tl + hl + 2 >= sizeof(buf))
			break;
		memcpy(buf + n, tag, tl);
		n += tl;
		memcpy(buf + n, hist[i].text, hl);
		n += hl;
		buf[n++] = '\n';
		buf[n] = '\0';
	}
	if (n == 0)
		return 0;

	memset(&input, 0, sizeof(input));
	input.role = "user";
	input.enable_thinking = false;
	input.input_type = RKLLM_INPUT_PROMPT;
	input.prompt_input = buf;

	memset(&infer, 0, sizeof(infer));
	infer.mode = RKLLM_INFER_GENERATE;
	infer.keep_history = 1;
	infer.max_new_tokens = 1; /* just to land prompt in KV; callback aborts */

	prefilling = 1;
	ret = p_run(handle, &input, &infer, NULL);
	prefilling = 0;
	if (ret != 0)
		fprintf(stderr, "rkllm_session: prefill hist failed (%d)\n", ret);
	else
		fprintf(stderr, "rkllm_session: prefill hist ok (%d turns)\n",
			hist_n - 1);
	return ret;
}

int session_compress(void)
{
	int i;

	if (!loaded || !model_path_saved[0] || compressing)
		return -1;
	/* Refuse while generate is in flight (UI path). Worker calls us only after run returns. */
	if (handle && p_is_running && p_is_running(handle))
		return -1;
	compressing = 1;
	session_abort();

	if (hist_n > 5) {
		struct hist_msg keep[HIST_MAX];
		int kn = 0;

		keep[kn++] = hist[0];
		for (i = hist_n - 4; i < hist_n; i++) {
			if (i >= 1)
				keep[kn++] = hist[i];
		}
		memcpy(hist, keep, sizeof(hist[0]) * (size_t)kn);
		hist_n = kn;
	}

	destroy_handle();
	if (create_handle(model_path_saved) != 0) {
		compressing = 0;
		tokq_push(TOKQ_ERROR, "压缩后重建失败");
		return -1;
	}
	loaded = 1;
	/* Was missing: recreate wiped KV; must prefill kept turns. */
	prefill_retained_hist();
	refresh_ctx();
	compressing = 0;
	tokq_push(TOKQ_STATUS, "[压缩完成]");
	return 0;
}

void session_get_stats(struct session_stats *out)
{
	if (!out)
		return;
	out->loaded = loaded;
	out->running = (handle && p_is_running) ? p_is_running(handle) : 0;
	out->ctx_tokens = last_ctx;
	out->max_context = LLM_PANEL_MAX_CONTEXT;
	out->token_per_s = last_tps;
}

const char *session_smoke_question(void)
{
	return SMOKE_Q;
}
