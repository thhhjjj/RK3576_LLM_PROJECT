/* SPDX-License-Identifier: GPL-2.0 */
#include "tokq.h"

#include <pthread.h>
#include <string.h>

#define TOKQ_CAP 128

static struct tokq_evt q[TOKQ_CAP];
static unsigned head, tail, count;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

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
}

int tokq_push(enum tokq_kind kind, const char *text)
{
	int rc = -1;

	pthread_mutex_lock(&lock);
	if (count < TOKQ_CAP) {
		q[tail].kind = kind;
		if (text) {
			strncpy(q[tail].text, text, sizeof(q[tail].text) - 1);
			q[tail].text[sizeof(q[tail].text) - 1] = '\0';
		} else {
			q[tail].text[0] = '\0';
		}
		tail = (tail + 1) % TOKQ_CAP;
		count++;
		rc = 0;
	}
	pthread_mutex_unlock(&lock);
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
