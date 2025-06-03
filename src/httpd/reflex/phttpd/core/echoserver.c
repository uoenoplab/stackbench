/*
 * Copyright 2013-16 Board of Trustees of Stanford University
 * Copyright 2013-16 Ecole Polytechnique Federale Lausanne (EPFL)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>

#include <ix/mempool.h>
#include <ixev.h>

#include <stdlib.h>

#define ROUND_UP(num, multiple) ((((num) + (multiple) - 1) / (multiple)) * (multiple))

static char *HTTPHDR = (char *)"HTTP/1.1 200 OK\r\n" // 17
                 "Connection: keep-alive\r\n" // 24
                 "Server: Apache/2.2.800\r\n" // 24
                 "Content-Length: "; // 16

char *response;

struct pp_conn {
	struct ixev_ctx ctx;
	size_t bytes_left;
	char data[];
};

static size_t msg_size;
//static size_t response_size;

static struct mempool_datastore pp_conn_datastore;
static __thread struct mempool pp_conn_pool;

static void pp_main_handler(struct ixev_ctx *ctx, unsigned int reason);

static void pp_stream_handler(struct ixev_ctx *ctx, unsigned int reason)
{
	struct pp_conn *conn = container_of(ctx, struct pp_conn, ctx);
	size_t bytes_so_far = msg_size - conn->bytes_left;
	ssize_t ret;

	ret = ixev_send(ctx, &conn->data[bytes_so_far], conn->bytes_left);
	if (ret < 0) {
		if (ret != -EAGAIN)
			ixev_close(ctx);
		return;
	}

	conn->bytes_left -= ret;
	if (!conn->bytes_left) {
		conn->bytes_left = msg_size;
		ixev_set_handler(ctx, IXEVIN, &pp_main_handler);
	}
}

static void pp_main_handler(struct ixev_ctx *ctx, unsigned int reason)
{
	struct pp_conn *conn = container_of(ctx, struct pp_conn, ctx);
	ssize_t ret;

	ret = ixev_recv(ctx, &conn->data, conn->bytes_left);
	if (ret <= 0) {
		if (ret != -EAGAIN) {
			ixev_close(ctx);
		}
		return;
	}

	ret = ixev_send(ctx, response, strlen(response));
	if (ret == -EAGAIN)
		ret = 0;
	if (ret < 0) {
		ixev_close(ctx);
		return;
	}
}

static struct ixev_ctx *pp_accept(struct ip_tuple *id)
{
	/* NOTE: we accept everything right now, did we want a port? */
	struct pp_conn *conn = mempool_alloc(&pp_conn_pool);
	if (!conn)
		return NULL;

	conn->bytes_left = msg_size;
	ixev_ctx_init(&conn->ctx);
	ixev_set_handler(&conn->ctx, IXEVIN, &pp_main_handler);

	return &conn->ctx;
}

static void pp_release(struct ixev_ctx *ctx)
{
	struct pp_conn *conn = container_of(ctx, struct pp_conn, ctx);

	mempool_free(&pp_conn_pool, conn);
}

static struct ixev_conn_ops pp_conn_ops = {
	.accept		= &pp_accept,
	.release	= &pp_release,
};

static void *pp_main(void *arg)
{
	int ret;

printf("pp_main on cpu %d\n", percpu_get(cpu_nr));
	ret = ixev_init_thread();
	if (ret) {
		fprintf(stderr, "unable to init IXEV\n");
		return NULL;
	};

	ret = mempool_create(&pp_conn_pool, &pp_conn_datastore, MEMPOOL_SANITY_GLOBAL, 0);
	if (ret) {
		fprintf(stderr, "unable to create mempool\n");
		return NULL;
	}

	while (1) {
		ixev_wait();
	}

	return NULL;
}

int echoserver_main(int argc, char *argv[])
{
	int i, nr_cpu;
	pthread_t tid;
	int ret;
	unsigned int pp_conn_pool_entries;

	if (argc < 1) {
		fprintf(stderr, "Usage: %s MSG_SIZE [MAX_CONNECTIONS]\n", argv[0]);
		return -1;
	}

	msg_size = atol(argv[1]);

	size_t payload_size = atol(argv[2]);
	response = calloc(91 + payload_size, sizeof(char));
	snprintf(response, 91 + payload_size, "%s%5ld\r\n\r\n", HTTPHDR, payload_size);
	size_t header_size = strlen(response);
	for (size_t i = header_size; i < header_size + payload_size; i++) {
		response[i] = 'A' + (random() % 26);
	}

	if (argc >= 4)
		pp_conn_pool_entries = atoi(argv[3]);
	else
		pp_conn_pool_entries = 16 * 4096;
	fprintf(stderr, "pool entries %d\n", pp_conn_pool_entries);

	pp_conn_pool_entries = ROUND_UP(pp_conn_pool_entries, MEMPOOL_DEFAULT_CHUNKSIZE);

	ret = ixev_init(&pp_conn_ops);
	if (ret) {
		fprintf(stderr, "failed to initialize ixev\n");
		return ret;
	}

	ret = mempool_create_datastore(&pp_conn_datastore, pp_conn_pool_entries, sizeof(struct pp_conn) + msg_size, "pp_conn");

	if (ret) {
		fprintf(stderr, "unable to create mempool\n");
		return ret;
	}

	nr_cpu = cpus_active;
	if (nr_cpu < 1) {
		fprintf(stderr, "got invalid cpu count %d\n", nr_cpu);
		exit(-1);
	}

	sys_spawnmode(true);

	for (i = 1; i < nr_cpu; i++) {
		fprintf(stderr, "main thread: starting cpu %d\n", i);
		ret = rte_eal_remote_launch(pp_main, (void *)(unsigned long) i, i);		
		if (ret) {
			fprintf(stderr, "failed to spawn thread %d\n", i);
			exit(-1);
		}
	}
	fprintf(stderr, "nr_cpu %d\n", nr_cpu);

	pp_main(NULL);
	return 0;
}

