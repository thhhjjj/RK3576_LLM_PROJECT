点「试问」到出回答，整条链路：

### 0. 启动时绑好（只做一次）

```177:177:kernel-6.1/markdown/project/llm_panel/main.c
	ui_set_actions(on_ui_compress, on_ui_ask);//设置UI操作回调函数
```

```85:88:kernel-6.1/markdown/project/llm_panel/ui.c
void ui_set_actions(ui_action_fn on_compress_fn, ui_action_fn on_ask_fn)
{
	act_compress = on_compress_fn;
	act_ask = on_ask_fn;
```

```188:188:kernel-6.1/markdown/project/llm_panel/main.c
	lv_timer_create(metrics_timer_cb, 100, NULL);//创建100ms的定时器回调
```

`act_ask` → `on_ui_ask`；100ms 定时器负责后面捞 `tokq`。

---

### 1. LVGL 点击

```142:142:kernel-6.1/markdown/project/llm_panel/ui.c
	lv_obj_add_event_cb(btn_ask, on_ask, LV_EVENT_CLICKED, NULL);
```

```54:58:kernel-6.1/markdown/project/llm_panel/ui.c
static void on_ask(lv_event_t *e)
{
	(void)e;
	if (act_ask)
		act_ask();
```

`on_ask` → `act_ask()` = `on_ui_ask`。

---

### 2. 主线程先画问句，再排队

```39:51:kernel-6.1/markdown/project/llm_panel/main.c
static void on_ui_ask(void)
{
	const char *q = session_smoke_question();
	...
	ui_append_dialog("\n你: ");
	ui_append_dialog(q);
	ui_append_dialog("\n助手: ");
	if (session_ask(q) != 0)
		ui_append_dialog("[忙或失败]\n");
}
```

预置句在：

```699:701:kernel-6.1/markdown/project/llm_panel/rkllm_session.c
const char *session_smoke_question(void)
{
	return SMOKE_Q;
```

---

### 3. `session_ask`：只投递，立刻返回

```563:586:kernel-6.1/markdown/project/llm_panel/rkllm_session.c
int session_ask(const char *user_text)
{
	...
	strncpy(pending_user, user_text, ...);
	have_job = 1;
	pthread_cond_signal(&wcond);
	...
	return 0;
}
```

---

### 4. Worker 醒来跑推理

```417:474:kernel-6.1/markdown/project/llm_panel/rkllm_session.c
static void *worker_main(void *arg)
{
	...
		while (!have_job && !worker_stop)
			pthread_cond_wait(&wcond, &wlock);
		...
		hist_push('u', user);
		...
			snprintf(prompt_buf, ..., "%s\n/no_think", user);
			...
			ret = p_run(handle, &input, &infer, NULL);
```

`p_run` = `rkllm_run`；阻塞在 worker，不堵 LVGL。

---

### 5. 回调只推 `tokq`（不画屏）

注册在 `create_handle`：

```396:397:kernel-6.1/markdown/project/llm_panel/rkllm_session.c
	cb.result_callback = result_cb;
```

流式出字 / 结束：

```292:292:kernel-6.1/markdown/project/llm_panel/rkllm_session.c
		tokq_push(TOKQ_TOKEN, result->text);
```

```325:325:kernel-6.1/markdown/project/llm_panel/rkllm_session.c
			tokq_push(TOKQ_FINISH, NULL);
```

实现：`tokq.c` 的 `tokq_push` / `tokq_pop`。

---

### 6. 主线程定时器捞队列、画回答

```89:96:kernel-6.1/markdown/project/llm_panel/main.c
static void metrics_timer_cb(lv_timer_t *t)
{
	...
	drain_tokq();
```

```54:68:kernel-6.1/markdown/project/llm_panel/main.c
static void drain_tokq(void)
{
	...
		case TOKQ_TOKEN:
			ui_append_dialog(ev.text);
			break;
		case TOKQ_FINISH:
			ui_append_dialog("\n");
			session_get_stats(&st);
			ui_set_ctx_tps(...);
```

---

**整条线（函数名）：**

`on_ask`（ui.c）→ `on_ui_ask`（main.c）→ `session_ask` → **`worker_main` / `p_run`** → **`result_cb` → `tokq_push`** → **`metrics_timer_cb` → `drain_tokq` → `ui_append_dialog`**