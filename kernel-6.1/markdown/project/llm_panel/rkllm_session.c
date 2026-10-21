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
static char model_path_saved[512];

static struct hist_msg hist[HIST_MAX];
static int hist_n;

static pthread_t worker;
static pthread_mutex_t wlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wcond = PTHREAD_COND_INITIALIZER;
static int worker_live;
static int worker_stop;
static int have_job;//有未复制内容
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

//重置对话历史，只保留系统提示词
static void hist_reset_keep_system(void)
{
	hist_n = 0;
	hist[0].role = 's';//system 主题与人设
	strncpy(hist[0].text, SYSTEM_PROMPT, MSG_MAX - 1);//复制系统提示词到hist[0].text
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
	//最大上下文窗口
	param.max_context_len = LLM_PANEL_MAX_CONTEXT;
	//单轮生成最大tokens数
	param.max_new_tokens = 512;
	//top_k，按排名采样，每次只固定填入最大概率的前k个token，且再从k个抽一个
	param.top_k = 1;
	//top_p，按范围概率采样，每次取n个token，直到概率和大于0.9，再从n个中抽一个
	param.top_p = 0.9f;

	/*temperature，
	 *emperature（温度）：在抽下一个 token 之前，把概率分布「拉平」或「拉尖」。
	 *低（如 0.2）：更偏向高概率词 → 更稳、更死板
	 *高（如 1.2）：分布更平均 → 更活、更易跑偏
	 *1.0 附近：相对接近模型原始分布
	*/
	param.temperature = 0.8f;

	/*repeat_penalty
	 *重复惩罚：已经出现过的 token，再被选中时概率会被压低，减轻翻来覆去同一句/同一个词。
	 *1.0：不惩罚
	 *>1（你这里 1.1）：轻度抑制重复，常见默认档
	 *太大：可能表达别扭、故意绕开常用词
	*/
	param.repeat_penalty = 1.1f;

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
	 * Keep a short system prompt. Note: set_chat_template disables the
	 * runtime's automatic enable_thinking handling — we still pass
	 * enable_thinking=false and append /no_think on every user turn.
	 */
	//挂入 system 模板
	if (p_set_chat_template)
		p_set_chat_template(handle, SYSTEM_PROMPT, "", "");
	return 0;
}

static void *worker_main(void *arg)
{
	(void)arg;
	//持锁进循环，等待推理任务
	pthread_mutex_lock(&wlock);
	while (!worker_stop) {
		char user[MSG_MAX];
		RKLLMInput input;
		RKLLMInferParam infer;
		int ret;
		//等待推理任务，have_job为1，且未worker_stop，则进入线程
		while (!have_job && !worker_stop)
		/*
		 *条件等待
		 *放开 wlock（否则 session_ask 拿不到锁，没法投递任务）
		 *线程 阻塞休眠，直到别人 pthread_cond_signal(&wcond)（session_ask / session_fini）
		 */
			pthread_cond_wait(&wcond, &wlock);
		//如果worker线程停止，退出循环
		if (worker_stop)
			break;

		//复制用户输入pending_user到user
		strncpy(user, pending_user, sizeof(user) - 1);
		user[sizeof(user) - 1] = '\0';
		have_job = 0;//搬移结束
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

// 初始化会话模块
int session_init(void)
{
	//重置对话角色内容 S,U,A，设定S
	hist_reset_keep_system();

	//初始化token事件队列
	tokq_init();

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
	if (!user_text || !user_text[0] || !loaded)//用户输入文本为空，或模型未加载，则返回失败
		return -1;

	pthread_mutex_lock(&wlock);//加锁，保护与worker_main线程并发共享的pending_user，have_job，worker_stop，wcond资源
	
	/* 
	 *handle:当前 RKLLM 会话句柄；NULL 表示还没 init / 已销毁
	 *p_is_running:dlsym 出来的函数指针，对应 so 里的 rkllm_is_running，判断p_is_running函数是否存在，可能为 NULL
	 *p_is_running(handle)=rkllm_is_running(handle):调用该函数,判断这个句柄是否仍在生成中
	 */
	if (handle && p_is_running && p_is_running(handle)) {
		if (p_abort)//如果p_abort函数存在，则中止模型运行
			p_abort(handle);
		pthread_mutex_unlock(&wlock);//解锁，释放资源，等worker线程把p_is_running的running状态清掉，
		usleep(50000);//等待50ms
		pthread_mutex_lock(&wlock);
	}
	//如果还在运行，中止未停止，且还有推送have_job任务，先非阻塞返回-1
	if (have_job || (handle && p_is_running && p_is_running(handle))) {
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
