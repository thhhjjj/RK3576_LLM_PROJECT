/* SPDX-License-Identifier: GPL-2.0 */
/*
 * PHASE2/7: dlopen librkllmrt；worker 跑 ask + 异步压缩摘要。
 * 回调只推 tokq，不碰 LVGL。
 */

#include "rkllm_session.h"
#include "npu_dvfs.h"

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
/* PHASE7-C：近轮原文条数（不含 s / m）；摘要生成上限 */
#define KEEP_RECENT 4
#define SUMMARY_MAX_NEW 256
/* PHASE7-D：单次 prefill 文本上限，超出则分段 keep_history 喂入 */
#define PREFILL_CHUNK 2048

static const char *SYSTEM_PROMPT =
	"你是小千问（Qwen3.5-2B），说话热情、轻松、有趣。"
	"用户是主人，回答请简明。"
	"直接回答，不要输出思维过程，不要输出 think / Thinking 标签。";

static const char *SMOKE_Q = "用一句话介绍 RK3576 上的 RKNPU。";

/* Min kept answer bytes before we treat trailing "Thinking Process" as junk. */
#define THINK_ANSWER_MIN 12

//文本属性
struct hist_msg {
	char role; //文本所属角色
	char text[MSG_MAX];//对话文本内容
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
static volatile int gen_inflight; /* PHASE7-D：生成未结束（含 abort 后至 FINISH） */
static char model_path_saved[512];

static struct hist_msg hist[HIST_MAX];
static int hist_n;

static pthread_t worker;
static pthread_mutex_t wlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wcond = PTHREAD_COND_INITIALIZER;
static int worker_live;
static int worker_stop;
static int have_job; /* 有待推理的用户问 */
static int have_compress; /* PHASE7-C：异步压缩排队 */
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
 * skip_pre_think: model started with think block; wait until answer starts
 * (</think> / CJK / 非思维续写的正文)，不能只等中文——英文问句会被丢成空。
 */
static int drop_thinking;
static int skip_pre_think;
/* Compress rebuild: feed retained hist into new handle without showing tokens. */
static int prefilling;
/* PHASE7-C：摘要推理中，token 进 summary_acc，不刷对话区 */
static int summarizing;
static char summary_acc[MSG_MAX];
static size_t summary_acc_n;

static int compress_do_work(void);

//重置对话历史，只保留系统提示词（人设只在这里写入，之后只读）
static void hist_reset_keep_system(void)
{
	hist_n = 0;
	hist[0].role = 's';
	strncpy(hist[0].text, SYSTEM_PROMPT, MSG_MAX - 1);
	hist[0].text[MSG_MAX - 1] = '\0';
	hist_n = 1;
}

/* 摘要条：插在 hist[0] 之后；已有 'm' 则只换正文，绝不改 hist[0] */
static void hist_set_memory(const char *summary)
{
	char buf[MSG_MAX];

	if (!summary || !summary[0])
		return;
	snprintf(buf, sizeof(buf), "摘要: %s", summary);
	if (hist_n >= 2 && hist[1].role == 'm') {
		strncpy(hist[1].text, buf, MSG_MAX - 1);
		hist[1].text[MSG_MAX - 1] = '\0';
		return;
	}
	if (hist_n >= HIST_MAX) {
		/* 腾一位：丢掉最旧正文轮，不动 s */
		int i, drop = (hist_n > 2 && hist[1].role == 'm') ? 2 : 1;

		for (i = drop; i < hist_n - 1; i++)
			hist[i] = hist[i + 1];
		hist_n--;
	}
	{
		int i;

		for (i = hist_n; i > 1; i--)
			hist[i] = hist[i - 1];
		hist[1].role = 'm';
		strncpy(hist[1].text, buf, MSG_MAX - 1);
		hist[1].text[MSG_MAX - 1] = '\0';
		hist_n++;
	}
}

static void hist_push(char role, const char *text)
{
	if (role == 's' || role == 'm')
		return; /* 人设/摘要走专用路径 */
	if (hist_n >= HIST_MAX) {
		int i;
		int drop;

		if (hist_n <= 1)
			return;
		/* 永不挤掉 hist[0]；有摘要则从摘要后开始挤 */
		drop = (hist_n > 2 && hist[1].role == 'm') ? 2 : 1;
		for (i = drop; i < hist_n - 1; i++)
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

static const char *after_think_close(const char *s)
{
	static const char *ends[] = {
		"</think>",
		"</thinking>",
		NULL,
	};
	const char *best = NULL;
	int i;

	for (i = 0; ends[i]; i++) {
		const char *p = strstr(s, ends[i]);

		if (!p)
			continue;
		p += strlen(ends[i]);
		while (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t')
			p++;
		if (!best || p > best)
			best = p;
	}
	return best;
}

static int has_open_think_tag(const char *s)
{
	const char *open = strstr(s, "<think>");

	if (!open)
		return 0;
	return after_think_close(open) == NULL;
}

static const char *first_cjk_utf8(const char *s)
{
	const unsigned char *p = (const unsigned char *)s;

	while (*p) {
		if (p[0] >= 0xE4 && p[0] <= 0xE9 && p[1] && p[2])
			return (const char *)p;
		p++;
	}
	return NULL;
}

/*
 * Think-first 答案起点（流式）：只认闭合标签之后，或非 XML 的 Thinking 空行后。
 * 未闭合 <think> 在流式阶段返回 NULL，避免把思维里的中文提前刷上屏。
 */
static const char *answer_from_think_buf(const char *s)
{
	const char *after = after_think_close(s);
	char *mark;
	const char *p;

	if (after && *after)
		return after;
	if (has_open_think_tag(s))
		return NULL;

	mark = find_thinking_mark((char *)s);
	if (!mark)
		return first_cjk_utf8(s);
	/* XML 开标签未闭合已在上面处理；这里只处理纯文案 Thinking Process */
	if (mark[0] == '<')
		return NULL;
	p = strstr(mark, "\n\n");
	if (!p)
		return NULL;
	p += 2;
	while (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t')
		p++;
	return *p ? p : NULL;
}

/* 把 ans 挪到 reply_acc 开头 */
static void reply_keep_from(const char *ans)
{
	size_t len;

	if (!ans || !ans[0]) {
		reply_acc_n = 0;
		reply_acc[0] = '\0';
		return;
	}
	if (ans >= reply_acc && ans < reply_acc + sizeof(reply_acc)) {
		len = strlen(ans);
		memmove(reply_acc, ans, len + 1);
		reply_acc_n = len;
		return;
	}
	len = strlen(ans);
	if (len >= sizeof(reply_acc))
		len = sizeof(reply_acc) - 1;
	memcpy(reply_acc, ans, len);
	reply_acc[len] = '\0';
	reply_acc_n = len;
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

/* 去掉开头空白，以及尾部泄漏的 assistant/user 角色名 */
static void strip_role_noise(void)
{
	size_t i, n;
	int again = 1;
	static const char *roles[] = { "assistant", "Assistant", "user",
				       "User", "system", "System", NULL };

	n = reply_acc_n;
	i = 0;
	while (i < n &&
	       (reply_acc[i] == '\n' || reply_acc[i] == '\r' ||
		reply_acc[i] == ' ' || reply_acc[i] == '\t'))
		i++;
	if (i > 0) {
		memmove(reply_acc, reply_acc + i, n - i + 1);
		n -= i;
		reply_acc_n = n;
	}

	while (again) {
		int r;

		again = 0;
		n = reply_acc_n;
		while (n > 0 &&
		       (reply_acc[n - 1] == '\n' || reply_acc[n - 1] == '\r' ||
			reply_acc[n - 1] == ' ' || reply_acc[n - 1] == '\t')) {
			reply_acc[--n] = '\0';
			again = 1;
		}
		reply_acc_n = n;
		for (r = 0; roles[r]; r++) {
			size_t rn = strlen(roles[r]);

			if (n >= rn &&
			    memcmp(reply_acc + n - rn, roles[r], rn) == 0) {
				char before = (n > rn) ? reply_acc[n - rn - 1] :
							 '\n';

				if (before == '\n' || before == '\r' ||
				    before == ' ' || before == '\t' || n == rn) {
					n -= rn;
					reply_acc[n] = '\0';
					reply_acc_n = n;
					again = 1;
					break;
				}
			}
		}
	}
}

/*
 * FINISH 兜底：Qwen3.5 常只开 <think> 不关就把答案写在里面，
 * 流式抽不出 → 结束时再剥标签 / 取首个 CJK。
 */
static int salvage_reply_from_think(void)
{
	const char *ans;
	const char *p;
	const char *cjk;
	char *mark;

	ans = answer_from_think_buf(reply_acc);
	if (ans && *ans) {
		reply_keep_from(ans);
		strip_role_noise();
		return reply_acc_n > 0;
	}

	mark = find_thinking_mark(reply_acc);
	if (mark && mark[0] == '<') {
		p = mark + strlen("<think>");
		while (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t')
			p++;
		cjk = first_cjk_utf8(p);
		if (cjk)
			reply_keep_from(cjk);
		else if (*p)
			reply_keep_from(p);
		else
			return 0;
		strip_role_noise();
		return reply_acc_n > 0;
	}
	if (mark && mark[0] != '<') {
		p = strstr(mark, "\n\n");
		if (p) {
			p += 2;
			while (*p == '\n' || *p == '\r' || *p == ' ' ||
			       *p == '\t')
				p++;
			if (*p) {
				reply_keep_from(p);
				strip_role_noise();
				return reply_acc_n > 0;
			}
		}
		cjk = first_cjk_utf8(mark);
		if (cjk) {
			reply_keep_from(cjk);
			strip_role_noise();
			return reply_acc_n > 0;
		}
	}

	cjk = first_cjk_utf8(reply_acc);
	if (cjk) {
		reply_keep_from(cjk);
		strip_role_noise();
		return reply_acc_n > 0;
	}
	return 0;
}

static int result_cb(RKLLMResult *result, void *userdata, LLMCallState state)
{
	(void)userdata;

	if (state == RKLLM_RUN_ERROR) {
		if (prefilling || summarizing)
			return 0;
		gen_inflight = 0;
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

		/* Prefill after compress: stop ASAP, never paint UI. */
		if (prefilling) {
			if (handle && p_abort)
				p_abort(handle);
			return 0;
		}
		/* PHASE7-C 摘要：只攒字，不上屏 */
		if (summarizing) {
			if (summary_acc_n + len < sizeof(summary_acc) - 1) {
				memcpy(summary_acc + summary_acc_n, result->text,
				       len);
				summary_acc_n += len;
				summary_acc[summary_acc_n] = '\0';
			}
			return 0;
		}

		pthread_mutex_lock(&reply_lock);
		if (drop_thinking) {
			pthread_mutex_unlock(&reply_lock);
			return 0;
		}

		/* Think-first: 继续攒，直到能抽出答案再上屏 */
		if (skip_pre_think) {
			const char *ans;

			if (reply_acc_n + len < sizeof(reply_acc) - 1) {
				memcpy(reply_acc + reply_acc_n, result->text,
				       len);
				reply_acc_n += len;
				reply_acc[reply_acc_n] = '\0';
			}
			ans = answer_from_think_buf(reply_acc);
			if (!ans || !ans[0]) {
				pthread_mutex_unlock(&reply_lock);
				return 0;
			}
			skip_pre_think = 0;
			reply_keep_from(ans);
			gen_tokens++;
			pthread_mutex_unlock(&reply_lock);
			tokq_push(TOKQ_TOKEN, reply_acc);
			fprintf(stderr,
				"rkllm_session: think→answer len=%zu\n",
				strlen(reply_acc));
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
			const char *ans;

			while (keep > 0 &&
			       (reply_acc[keep - 1] == '\n' ||
				reply_acc[keep - 1] == '\r' ||
				reply_acc[keep - 1] == ' '))
				keep--;

			if (keep < THINK_ANSWER_MIN) {
				/* 仅当已见到闭合标签后的正文才立刻上屏 */
				ans = answer_from_think_buf(reply_acc);
				if (ans && *ans && after_think_close(reply_acc)) {
					skip_pre_think = 0;
					reply_keep_from(ans);
					pthread_mutex_unlock(&reply_lock);
					tokq_push(TOKQ_TOKEN, reply_acc);
					fprintf(stderr,
						"rkllm_session: skip leading think (inline answer)\n");
					return 0;
				}
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

		if (prefilling || summarizing)
			return 0;

		gen_inflight = 0;
		clock_gettime(CLOCK_MONOTONIC, &t1);
		sec = (t1.tv_sec - gen_t0.tv_sec) +
		      (t1.tv_nsec - gen_t0.tv_nsec) / 1e9;
		if (sec > 0.05 && gen_tokens > 0)
			last_tps = (float)gen_tokens / (float)sec;

		{
			int empty;
			int salvage = 0;
			char salvage_buf[MSG_MAX];

			salvage_buf[0] = '\0';
			pthread_mutex_lock(&reply_lock);
			/*
			 * Think-first 若一直没抽出答案：FINISH 再抽一次。
			 * 必须先于 trim_thinking_tail（它会砍掉 mark 之后的正文）。
			 */
			if (skip_pre_think && reply_acc_n) {
				if (salvage_reply_from_think()) {
					salvage = 1;
					fprintf(stderr,
						"rkllm_session: finish salvage answer len=%zu head='%.48s'\n",
						reply_acc_n, reply_acc);
				} else {
					fprintf(stderr,
						"rkllm_session: finish think dump len=%zu head='%.80s'\n",
						reply_acc_n, reply_acc);
					reply_acc_n = 0;
					reply_acc[0] = '\0';
				}
				skip_pre_think = 0;
			}
			trim_thinking_tail();
			strip_role_noise();
			/* 仍空：再试一次从残留缓冲抽（非 skip 路径也可能只剩标签） */
			if (reply_acc_n == 0) {
				/* already cleared */
			} else if (find_thinking_mark(reply_acc) == reply_acc) {
				if (salvage_reply_from_think()) {
					salvage = 1;
					fprintf(stderr,
						"rkllm_session: post-trim salvage len=%zu\n",
						reply_acc_n);
				}
			}
			empty = (reply_acc_n == 0);
			if (!empty) {
				hist_push('a', reply_acc);
				if (salvage) {
					strncpy(salvage_buf, reply_acc,
						sizeof(salvage_buf) - 1);
					salvage_buf[sizeof(salvage_buf) - 1] =
						'\0';
				}
			} else {
				fprintf(stderr,
					"rkllm_session: empty reply after filters\n");
			}
			reply_acc_n = 0;
			reply_acc[0] = '\0';
			drop_thinking = 0;
			skip_pre_think = 0;
			pthread_mutex_unlock(&reply_lock);

			if (salvage && salvage_buf[0])
				tokq_push(TOKQ_TOKEN, salvage_buf);
			if (empty)
				tokq_push(TOKQ_STATUS, "[无有效回答]");
			tokq_push(TOKQ_FINISH, NULL);
		}
		return 0;
	}
	return 0;
}

/*
* 解析符号：把 librkllmrt.so 里 rkllm_xxx 的函数指针都封装出来。
* 缺依赖：解析时失败，但 dlsym() 本身会返回 NULL。
* 返回：0 成功，-1 失败
*/
static int resolve_syms(void)
{
	p_createDefaultParam = (fn_createDefaultParam)dlsym(lib, "rkllm_createDefaultParam");
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

/*
* 静态库与动态库：llm_panel 编译时不强制 staging 里有 RKLLM；
* 没有 so 也能编过、UI 先起来，只是推理不可用。若链成静态库，没库就链失败，
* 或把整份 runtime 焊死进二进制。
*/
//open_lib：把 RKLLM 运行时 .so 装进进程，并解析出要用的函数指针
static int open_lib(void)
{
	//从环境变量中获取库路径
	const char *path = getenv("LLM_PANEL_LIB");
	//如果环境变量中没有库路径，使用默认库路径
	if (!path || !path[0])
		path = LLM_PANEL_DEFAULT_LIB;
	//RTLD_NOW 立刻解析库里的符号依赖；缺依赖现在就失败
	lib = dlopen(path, RTLD_NOW);
	if (!lib) {
		fprintf(stderr, "rkllm_session: dlopen %s failed: %s\n", path,
			dlerror());
		return -1;
	}
	//把 librkllmrt.so 里 rkllm_xxx 的函数指针都封装出来。
	return resolve_syms();
}

//销毁会话句柄
static void destroy_handle(void)
{
	if (handle && p_destroy) {
		p_abort(handle);
		p_destroy(handle);
	}
	handle = NULL;
	loaded = 0;
}

//初始化 RKLLM 的 LLMHandle，并把模型加载进去。
//参数：模型路径
static int create_handle(const char *model_path)
{
	RKLLMParam param;
	RKLLMCallback cb;
	int ret;
	//导入默认值
	param = p_createDefaultParam();
	//设置权重模型路径
	param.model_path = model_path;
	/* 最大上下文：须 ≤ .rkllm 导出时的 max_context_len */
	param.max_context_len = LLM_PANEL_MAX_CONTEXT;
	fprintf(stderr, "rkllm_session: max_context_len=%d\n",
		LLM_PANEL_MAX_CONTEXT);
	//单轮生成最大tokens数
	param.max_new_tokens = 512;
	/* top_k=1 等于几乎贪心，温度再高也闷；抬高以增加随机性 */
	param.top_k = 40;
	param.top_p = 0.95f;
	/* 略高于 1：更活、更有趣味；再高易胡言 */
	param.temperature = 1.15f;
	/* 轻度防复读即可，太高会说话别扭 */
	param.repeat_penalty = 1.05f;

	/*skip_special_token
	 *跳过特殊 token：生成结果里若出现模型内部用的特殊符号（如结束符、聊天模板标记、某些控制 token），不要把它们当作普通文字回给你。
	 *true：回调/result->text 里尽量只有可读内容，适合直接刷到对话框。
	 *false：特殊 token 也可能露出来，一般调试或特殊协议才用。
	 *板端 UI 用 true 是合理默认。
	*/
	param.skip_special_token = true;

	/*is_async
	 *是否异步：让 RKLLM 后台偷偷跑，回调函数不阻塞 UI。
	 *true：回调不阻塞 UI，UI 照常刷。但会慢一拍：结果出来时 UI 可能已经显示下一轮了。
	 *false：回调阻塞 UI，UI 等它回来才刷。但会卡顿：结果出来前 UI 动不了。
	 *本项目单独起了worker线程，阻塞读不影响主线程
	 */
	param.is_async = false;

	//注册推理结果回调
	memset(&cb, 0, sizeof(cb));
	cb.result_callback = result_cb;
	//回调函数私有结构体，单会话无需，NULL 即可
	cb.result_userdata = NULL;

	//初始化模型
	handle = NULL;
	ret = p_init(&handle, &param, &cb);
	if (ret != 0 || !handle) {
		fprintf(stderr, "rkllm_session: rkllm_init failed (%d)\n", ret);
		handle = NULL;
		return -1;
	}
	/*
	 * ChatML。prefix/postfix 不能为空，否则 runtime
	 * 当裸续写：会复读人设自我介绍，并把「用户句+/no_think」吐回屏上。
	 * set_chat_template 会关掉自动 thinking 处理 → 仍 enable_thinking=false
	 * 且每轮 user 后追加 /no_think。
	 */
	if (p_set_chat_template) {
		int tr = p_set_chat_template(handle, SYSTEM_PROMPT,
					    "<|im_start|>user\n",
					    "<|im_end|>\n<|im_start|>assistant\n");

		fprintf(stderr,
			"rkllm_session: chat_template ChatML ret=%d\n", tr);
	}
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

		while (!have_job && !have_compress && !worker_stop)
			pthread_cond_wait(&wcond, &wlock);
		if (worker_stop)
			break;

		/* 压缩优先于新问，避免水位与手动压缩饿死 */
		if (have_compress) {
			have_compress = 0;
			pthread_mutex_unlock(&wlock);
			compress_do_work();
			pthread_mutex_lock(&wlock);
			continue;
		}

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
		gen_inflight = 1;

		/*
		 * Qwen3.5 官方不支持 /think|/no_think 软开关；set_chat_template
		 * 也会关掉 runtime 对 enable_thinking 的自动处理。靠人设禁思维，
		 * 并在回调里剥 think 标签。用户原文原样送入，勿追加 /no_think。
		 */
		fprintf(stderr, "rkllm_session: ask len=%zu head='%.48s'\n",
			strlen(user), user);
		memset(&input, 0, sizeof(input));
		input.role = "user";
		input.enable_thinking = false;
		input.input_type = RKLLM_INPUT_PROMPT;
		input.prompt_input = user;

		memset(&infer, 0, sizeof(infer));
		infer.mode = RKLLM_INFER_GENERATE;
		infer.keep_history = 1;

		/* PHASE7-A：升频 → run → 降回（勿在回调里改频） */
		npu_dvfs_boost();
		ret = p_run(handle, &input, &infer, NULL);
		npu_dvfs_restore();
		gen_inflight = 0;
		if (ret != 0)
			tokq_push(TOKQ_ERROR, "rkllm_run 失败");

		refresh_ctx();
		if (last_ctx > 0 &&
		    last_ctx * 100 >=
			    LLM_PANEL_MAX_CONTEXT * LLM_PANEL_COMPRESS_WATER_PCT) {
			/* PHASE7-D：70% 也走异步队列，与按钮/低水位一致 */
			tokq_push(TOKQ_STATUS, "[自动压缩已排队]");
			pthread_mutex_lock(&wlock);
			have_compress = 1;
			pthread_mutex_unlock(&wlock);
		}

		pthread_mutex_lock(&wlock);
	}
	pthread_mutex_unlock(&wlock);
	return NULL;
}

// 初始化会话模块
int session_init(void)
{
	//重置对话角色内容 S,U,A，设定S
	hist_reset_keep_system();

	//初始化token事件队列
	tokq_init();

	npu_dvfs_init(); /* PHASE7-A：探测 NPU devfreq */

	//初始化worker线程状态参数
	worker_stop = 0;//worker停止标志
	have_job = 0;//worker是否有推理任务标志
	worker_live = 0;//worker线程是否创建

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

//加载会话模块
int session_load(const char *model_path)
{
	const char *path = model_path;
//路径为NULL，先获取模型路径--------------------------------
	if (!path || !path[0])
		//从环境变量中获取模型路径
		path = getenv("LLM_PANEL_MODEL");
		//如果环境变量中没有模型路径，使用默认模型路径
	if (!path || !path[0])
		path = LLM_PANEL_DEFAULT_MODEL;
//-------------------------------------------------------
	//复制模型路径到model_path_saved
	strncpy(model_path_saved, path, sizeof(model_path_saved) - 1);
	model_path_saved[sizeof(model_path_saved) - 1] = '\0';

	//如果lib为空，打开lib
	//dlopen("librkllmrt.so", ...)动态库，返回lib控制句柄
	if (!lib && open_lib() != 0)
		return -1;

	//开始拉起会话前，如果有会话句柄先销毁
	destroy_handle();

	hist_reset_keep_system();

	//创建会话句柄，加载模型，配置模型初始参数
	if (create_handle(path) != 0)
		return -1;

	loaded = 1;//设置模型已加载标志
	tokq_push(TOKQ_STATUS, "[模型已加载]");

	//如果worker线程未创建，创建worker线程
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

/*
 * 
 * 参数：用户输入文本
 * 返回：0 成功，-1 失败
*/
int session_ask(const char *user_text)
{
	if (!user_text || !user_text[0] || !loaded)
		return -1;

	pthread_mutex_lock(&wlock);
	if (compressing || have_compress || gen_inflight || have_job) {
		pthread_mutex_unlock(&wlock);
		return -1; /* 压缩/生成未结束：须先停干净再问 */
	}
	if (handle && p_is_running && p_is_running(handle)) {
		pthread_mutex_unlock(&wlock);
		return -1;
	}
	//模型未在运行，则复制用户输入文本到pending_user
	strncpy(pending_user, user_text, sizeof(pending_user) - 1);
	pending_user[sizeof(pending_user) - 1] = '\0';
	have_job = 1;
	pthread_cond_signal(&wcond);//唤醒worker线程，处理have_job
	pthread_mutex_unlock(&wlock);
	return 0;
}

void session_abort(void)
{
	if (handle && p_abort)
		p_abort(handle);
	/* gen_inflight 仍由 FINISH/ERROR 或 worker 清零 */
}

int session_gen_busy(void)
{
	return gen_inflight ||
	       (handle && p_is_running && p_is_running(handle)) ||
	       compressing || have_compress;
}

/* 喂一段 prefill（keep_history=1）；callback 里 prefilling 会立刻 abort 生成。 */
static int prefill_one_chunk(const char *chunk)
{
	RKLLMInput input;
	RKLLMInferParam infer;
	int ret;

	if (!chunk || !chunk[0] || !handle)
		return 0;
	memset(&input, 0, sizeof(input));
	input.role = "user";
	input.enable_thinking = false;
	input.input_type = RKLLM_INPUT_PROMPT;
	input.prompt_input = chunk;
	memset(&infer, 0, sizeof(infer));
	infer.mode = RKLLM_INFER_GENERATE;
	infer.keep_history = 1;
	infer.max_new_tokens = 1;
	prefilling = 1;
	npu_dvfs_boost();
	ret = p_run(handle, &input, &infer, NULL);
	npu_dvfs_restore();
	prefilling = 0;
	return ret;
}

/* After destroy+create：分段 prefill，降低一次灌满卡顿（PHASE7-D）. */
static int prefill_retained_hist(void)
{
	static char buf[PROMPT_MAX];
	size_t n = 0;
	int i;
	int chunks = 0;
	int ret = 0;

	if (!handle || hist_n <= 1)
		return 0;

	buf[0] = '\0';
	for (i = 1; i < hist_n; i++) {
		const char *tag;
		size_t tl, hl, need;

		if (hist[i].role == 'u')
			tag = "用户: ";
		else if (hist[i].role == 'a')
			tag = "助手: ";
		else if (hist[i].role == 'm')
			tag = "";
		else
			continue;
		tl = strlen(tag);
		hl = strlen(hist[i].text);
		need = tl + hl + 1;
		if (n > 0 && n + need >= PREFILL_CHUNK) {
			ret = prefill_one_chunk(buf);
			chunks++;
			if (ret != 0)
				break;
			n = 0;
			buf[0] = '\0';
		}
		if (n + need >= sizeof(buf))
			break;
		if (tl) {
			memcpy(buf + n, tag, tl);
			n += tl;
		}
		memcpy(buf + n, hist[i].text, hl);
		n += hl;
		buf[n++] = '\n';
		buf[n] = '\0';
	}
	if (ret == 0 && n > 0) {
		ret = prefill_one_chunk(buf);
		chunks++;
	}
	if (ret != 0)
		fprintf(stderr, "rkllm_session: prefill hist failed (%d)\n", ret);
	else
		fprintf(stderr,
			"rkllm_session: prefill hist ok (%d turns, %d chunk)\n",
			hist_n - 1, chunks);
	return ret;
}

/*
 * PHASE7-C 实际压缩（须在 worker 线程调用）：
 * 人设不动 → 旧轮摘要成 'm' → 留近轮 → destroy/create/prefill。
 */
static int compress_do_work(void)
{
	struct hist_msg keep[HIST_MAX];
	int kn = 0;
	int i;
	int body0;
	int keep_from;
	char discard[PROMPT_MAX];
	size_t dn = 0;
	char summary[MSG_MAX];

	if (!loaded || !model_path_saved[0])
		return -1;
	if (compressing)
		return -1;
	compressing = 1;
	session_abort();
	tokq_push(TOKQ_STATUS, "[摘要压缩中…]");

	/* hist[0] 人设只复制，永不改写内容 */
	keep[kn++] = hist[0];

	body0 = (hist_n >= 2 && hist[1].role == 'm') ? 2 : 1;
	keep_from = hist_n - KEEP_RECENT;
	if (keep_from < body0)
		keep_from = body0;

	discard[0] = '\0';
	for (i = body0; i < keep_from; i++) {
		const char *tag = (hist[i].role == 'u') ? "用户: " :
				  (hist[i].role == 'a') ? "助手: " : "";
		size_t tl = strlen(tag);
		size_t hl = strlen(hist[i].text);

		if (dn + tl + hl + 2 >= sizeof(discard))
			break;
		if (tl) {
			memcpy(discard + dn, tag, tl);
			dn += tl;
		}
		memcpy(discard + dn, hist[i].text, hl);
		dn += hl;
		discard[dn++] = '\n';
		discard[dn] = '\0';
	}

	summary[0] = '\0';
	if (dn > 0 && handle && p_run) {
		static char prompt[PROMPT_MAX];
		RKLLMInput input;
		RKLLMInferParam infer;
		int ret;

		snprintf(prompt, sizeof(prompt),
			 "请用简洁中文总结下列对话：保留要点、结论与未决问题，"
			 "不超过200字。不要输出思维过程或标签。\n\n%s",
			 discard);
		summary_acc_n = 0;
		summary_acc[0] = '\0';
		summarizing = 1;
		memset(&input, 0, sizeof(input));
		input.role = "user";
		input.enable_thinking = false;
		input.input_type = RKLLM_INPUT_PROMPT;
		input.prompt_input = prompt;
		memset(&infer, 0, sizeof(infer));
		infer.mode = RKLLM_INFER_GENERATE;
		infer.keep_history = 0;
		infer.max_new_tokens = SUMMARY_MAX_NEW;
		npu_dvfs_boost();
		ret = p_run(handle, &input, &infer, NULL);
		npu_dvfs_restore();
		summarizing = 0;
		if (ret == 0 && summary_acc_n > 0) {
			strncpy(summary, summary_acc, sizeof(summary) - 1);
			summary[sizeof(summary) - 1] = '\0';
		} else {
			fprintf(stderr,
				"rkllm_session: summary run failed (%d), hard-trim\n",
				ret);
		}
	}

	/* 保留旧摘要作底，再被新摘要覆盖 */
	if (body0 == 2)
		keep[kn++] = hist[1];
	for (i = keep_from; i < hist_n; i++)
		keep[kn++] = hist[i];
	memcpy(hist, keep, sizeof(hist[0]) * (size_t)kn);
	hist_n = kn;
	/* 人设仍是 hist[0]，与 SYSTEM_PROMPT 一致（未改写） */
	if (summary[0])
		hist_set_memory(summary);

	destroy_handle();
	if (create_handle(model_path_saved) != 0) {
		compressing = 0;
		tokq_push(TOKQ_ERROR, "压缩后重建失败");
		return -1;
	}
	loaded = 1;
	prefill_retained_hist();
	refresh_ctx();
	compressing = 0;
	tokq_push(TOKQ_STATUS, summary[0] ? "[压缩完成·已更新摘要]" : "[压缩完成]");
	fprintf(stderr, "rkllm_session: compress done hist_n=%d has_mem=%d\n",
		hist_n, (hist_n >= 2 && hist[1].role == 'm'));
	return 0;
}

int session_compress(void)
{
	if (!loaded || !model_path_saved[0] || !worker_live)
		return -1;

	pthread_mutex_lock(&wlock);
	if (compressing || have_compress) {
		pthread_mutex_unlock(&wlock);
		return -1;
	}
	/* 生成中：先 abort，worker 空闲后再压 */
	if (handle && p_is_running && p_is_running(handle) && p_abort)
		p_abort(handle);
	have_compress = 1;
	pthread_cond_signal(&wcond);
	pthread_mutex_unlock(&wlock);
	tokq_push(TOKQ_STATUS, "[压缩已排队]");
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
