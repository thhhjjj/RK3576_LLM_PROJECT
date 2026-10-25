/* SPDX-License-Identifier: GPL-2.0 */
#include "tokq.h"

#include <pthread.h>
#include <string.h>

#define TOKQ_CAP 128

static struct tokq_evt q[TOKQ_CAP];
static unsigned head, tail, count;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static tokq_fanout_fn fanout_fn;

int tokq_init(void)
{
	pthread_mutex_lock(&lock);
	head = tail = count = 0;
	pthread_mutex_unlock(&lock);
	return 0;
}

void tokq_fini(void)
{
	tokq_init();
	fanout_fn = NULL;
}

void tokq_set_fanout(tokq_fanout_fn fn)
{
	fanout_fn = fn;
}

int tokq_push(enum tokq_kind kind, const char *text)
{
	struct tokq_evt ev;
	int rc = -1;

	ev.kind = kind;
	if (text) {
		strncpy(ev.text, text, sizeof(ev.text) - 1);
		ev.text[sizeof(ev.text) - 1] = '\0';
	} else {
		ev.text[0] = '\0';
	}

	pthread_mutex_lock(&lock);
	if (count < TOKQ_CAP) {
		q[tail] = ev;
		tail = (tail + 1) % TOKQ_CAP;
		count++;
		rc = 0;
	}
	pthread_mutex_unlock(&lock);

	/* Fan-out even if UI queue was full — WS clients stay independent. */
	if (fanout_fn)
		fanout_fn(&ev);
	return rc;
}

int tokq_pop(struct tokq_evt *out)
{
	int rc = 0;

	if (!out)
		return 0;
	pthread_mutex_lock(&lock);
	if (count > 0) {
		*out = q[head];
		head = (head + 1) % TOKQ_CAP;
		count--;
		rc = 1;
	}
	pthread_mutex_unlock(&lock);
	return rc;
}
