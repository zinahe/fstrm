/*
 * Copyright (c) 2013-2014 by Farsight Security, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>
#include <inttypes.h>
#include <locale.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "libmy/my_alloc.h"
#include "libmy/my_time.h"

#include "libmy/my_queue.h"
#include "libmy/my_queue.c"

struct producer_stats {
	uint64_t	count_producer_full;
	uint64_t	count_producer;
	uint64_t	checksum_producer;
	uint64_t	count_insert_calls;
};

struct consumer_stats {
	uint64_t	count_consumer_empty;
	uint64_t	count_consumer;
	uint64_t	count_remove_calls;
	uint64_t	checksum_consumer;
};

enum wait_type {
	wt_spin,
	wt_slow_producer,
	wt_slow_consumer,
};

static volatile bool shut_down;

static enum wait_type wtype;
static const char *wtype_s;

static unsigned seconds;

static struct my_queue *q;

static inline void
maybe_wait_producer(int64_t i)
{
	if (wtype != wt_slow_producer)
		return;
	if ((i % 128) == 0) {
		struct timespec ts_wait = {
			.tv_sec = 0, .tv_nsec = 1
		};
		my_nanosleep(&ts_wait);
	}
}

static inline void
maybe_wait_consumer(int64_t i)
{
	if (wtype != wt_slow_consumer)
		return;
	if ((i % 128) == 0) {
		struct timespec ts_wait = {
			.tv_sec = 0, .tv_nsec = 1
		};
		my_nanosleep(&ts_wait);
	}
}

static void *
thr_producer(void *arg)
{
	bool res;
	void *item;
	unsigned space = 0;

	struct producer_stats *s;
	s = my_calloc(1, sizeof(*s));

	for (unsigned loops = 1; ; loops++) {
		for (int64_t i = 1; i <= 1000000; i++) {
			if (shut_down)
				goto out;
			item = (void *) i;

			res = my_queue_insert(q, item, &space);
			s->count_insert_calls++;
			if (res) {
				s->count_producer++;
				s->checksum_producer += i;
			} else {
				s->count_producer_full++;
			}
			maybe_wait_producer(i);
		}
	}
out:
	fprintf(stderr, "%s: producer thread shutting down\n", __func__);
	fprintf(stderr, "%s: count_producer= %" PRIu64 "\n", __func__, s->count_producer);
	fprintf(stderr, "%s: count_producer_full= %" PRIu64 "\n", __func__, s->count_producer_full);
	fprintf(stderr, "%s: count_insert_calls= %" PRIu64 "\n", __func__, s->count_insert_calls);
	fprintf(stderr, "%s: checksum_producer= %" PRIu64 "\n", __func__, s->checksum_producer);
	return (s);
}

static void *
thr_consumer(void *arg)
{
	bool res;
	void *item;
	unsigned count = 0;

	struct consumer_stats *s;
	s = my_calloc(1, sizeof(*s));

	for (unsigned loops = 1; ; loops++) {
		for (int64_t i = 1; i <= 1000000; i++) {
			res = my_queue_remove(q, &item, &count);
			s->count_remove_calls++;
			if (res) {
				int64_t v = (int64_t) item;
				if (v == 0) {
					fprintf(stderr, "%s: received shutdown message\n", __func__);
					goto out;
				}
				s->checksum_consumer += v;
				s->count_consumer++;
			} else {
				s->count_consumer_empty++;
			}
			maybe_wait_consumer(i);
		}
	}
out:
	fprintf(stderr, "%s: count_consumer= %" PRIu64 "\n", __func__, s->count_consumer);
	fprintf(stderr, "%s: count_consumer_empty= %" PRIu64 "\n", __func__, s->count_consumer_empty);
	fprintf(stderr, "%s: count_remove_calls= %" PRIu64 "\n", __func__, s->count_remove_calls);
	fprintf(stderr, "%s: checksum_consumer= %" PRIu64 "\n", __func__, s->checksum_consumer);
	return (s);
}

static void
send_shutdown_message(struct my_queue *my_q)
{
	void *item = (void *) 0;
	while(!my_queue_insert(my_q, item, NULL));
}

static void
check_stats(struct producer_stats *ps, struct consumer_stats *cs)
{
	if (ps->checksum_producer != cs->checksum_consumer) {
		printf("FATAL ERROR: producer checksum != consumer checksum "
		       "(%" PRIu64 " != %" PRIu64 ")\n",
		       ps->checksum_producer,
		       cs->checksum_consumer
		);
	}
	if (ps->count_producer != cs->count_consumer) {
		printf("FATAL ERROR: producer count != consumer count "
		       "(%" PRIu64 " != %" PRIu64 ")\n",
		       ps->count_producer,
		       cs->count_consumer
		);
	}
}

static void
print_stats(struct timespec *a, struct timespec *b,
	    struct producer_stats *ps, struct consumer_stats *cs)
{
	double dur;
	dur = my_timespec_to_double(b) - my_timespec_to_double(a);

	printf("%s: ran for %'.4f seconds in %s mode\n", __func__, dur, wtype_s);
	printf("%s: producer: %'.0f iter/sec [%d nsec/iter] (%.2f%% full)\n",
	       __func__,
	       ps->count_insert_calls / dur,
	       (int) (1E9 * dur / ps->count_insert_calls),
	       100.0 * ps->count_producer_full / ps->count_insert_calls
	);
	printf("%s: consumer: %'.0f iter/sec [%d nsec/iter] (%.2f%% empty)\n",
	       __func__,
	       cs->count_remove_calls / dur,
	       (int) (1E9 * dur / cs->count_remove_calls),
	       100.0 * cs->count_consumer_empty / cs->count_remove_calls
	);
}

int
main(int argc, char **argv)
{
	setlocale(LC_ALL, "");
	struct timespec ts_a, ts_b;
	struct producer_stats *ps;
	struct consumer_stats *cs;
	int size;

	if (argc != 4) {
		fprintf(stderr, "Usage: %s <slow_producer | slow_consumer | spin> <QUEUE SIZE> <RUN SECONDS>\n", argv[0]);
		return (EXIT_FAILURE);
	}
	if (strcasecmp(argv[1], "slow_producer") == 0) {
		wtype = wt_slow_producer;
		wtype_s = "slow producer";
	} else if (strcasecmp(argv[1], "slow_consumer") == 0) {
		wtype = wt_slow_consumer;
		wtype_s = "slow consumer";
	} else if (strcasecmp(argv[1], "spin") == 0) {
		wtype = wt_spin;
		wtype_s = "spin";
	} else {
		fprintf(stderr, "Error: invalid wait type '%s'\n", argv[1]);
		return (EXIT_FAILURE);
	}
	size = atoi(argv[2]);
	seconds = atoi(argv[3]);
	struct timespec ts = { .tv_sec = seconds, .tv_nsec = 0 };

	q = my_queue_init(size);
	if (q == NULL) {
		fprintf(stderr, "my_queue_init() failed, size too small or not a power-of-2?\n");
		return (EXIT_FAILURE);
	}
	printf("%s: queue implementation type: %s\n", argv[0], my_queue_impl_type());
	printf("%s: queue size: %d entries\n", argv[0], size);
	printf("%s: running for %d seconds\n", argv[0], seconds);

	pthread_t thr_p;
	pthread_t thr_c;

	my_gettime(CLOCK_MONOTONIC, &ts_a);

	pthread_create(&thr_p, NULL, thr_producer, NULL);
	pthread_create(&thr_c, NULL, thr_consumer, NULL);

	my_nanosleep(&ts);
	shut_down = true;

	pthread_join(thr_p, (void **) &ps);
	send_shutdown_message(q);
	pthread_join(thr_c, (void **) &cs);

	my_gettime(CLOCK_MONOTONIC, &ts_b);

	check_stats(ps, cs);
	print_stats(&ts_a, &ts_b, ps, cs);

	free(ps);
	free(cs);

	my_queue_destroy(&q);

	return 0;
}