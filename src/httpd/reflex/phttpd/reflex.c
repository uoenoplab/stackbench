#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <inttypes.h>
#include <sys/poll.h>
#include <sys/epoll.h>
#ifdef __FreeBSD__
#include <sys/event.h>
#include <sys/stat.h>
#endif /* __FreeBSD__ */
#include <net/if.h>
#include <netinet/tcp.h>	/* SOL_TCP */
#include <netinet/in.h>
#include <dirent.h>
#include <x86intrin.h>
#include <pthread.h>

#include <ix/mempool.h>
#include <ixev.h>

#include <stdlib.h>

#define ROUND_UP(num, multiple) ((((num) + (multiple) - 1) / (multiple)) * (multiple))

struct pp_conn {
        struct ixev_ctx ctx;
        int written;
	char *response_buf;
  int httplen;
};

static struct mempool_datastore pp_conn_datastore;
static __thread struct mempool pp_conn_pool;

#include "reflex.h"
#include "phttpd.h"

#define MAX_FLOW_NUM  (10000)
#define MAX_EVENTS (MAX_FLOW_NUM * 3)
#define TRACE_ERROR(str) fprintf(stderr, str)
#define PST_NAME	"pst:0"

#define DEBUG printf("debug: %s %d\n", __func__, __LINE__)

struct reflex_ctx {
  int *fds;
  struct phttpd_global *pg;
  struct nm_targ *targs;
} rc;

static char *HTTPHDR = (char *)"HTTP/1.1 200 OK\r\n" // 17
                 "Connection: keep-alive\r\n" // 24
                 "Server: Apache/2.2.800\r\n" // 24
                 "Content-Length: "; // 16

static void pp_main_handler(struct ixev_ctx *ctx, unsigned int reason);

static void pp_stream_handler(struct ixev_ctx *ctx, unsigned int reason)
{
	struct pp_conn *conn = container_of(ctx, struct pp_conn, ctx);
	struct nm_targ *t = &rc.targs[percpu_get(cpu_nr)];
	struct phttpd_global *pg = (struct phttpd_global *)
		t->g->garg_private;
	size_t msglen = pg->msglen;
	int httplen = conn->httplen;
        ssize_t len;
	len = ixev_send(ctx, conn->response_buf + conn->written, httplen + msglen - conn->written);
	if (unlikely(len <= 0)) {
	  perror("write");
	  exit(1);
	} else {
	  conn->written += len;
	}
	if (httplen + msglen <= conn->written) {
	  ixev_set_handler(ctx, IXEVIN, &pp_main_handler);
	}
}

extern char *data_buf;

int tmp_cnt[48];
static void
sigint_h(int sig)
{
	int i;

	(void)sig;	/* UNUSED */
	D("received control-C on thread %p", (void *)pthread_self());
	for (i = 0; i < 48; i++) {
	  printf("%d:%d ", i, tmp_cnt[i]);
	}
	printf("\n");
}
static void pp_main_handler(struct ixev_ctx *ctx, unsigned int reason)
{
        struct pp_conn *conn = container_of(ctx, struct pp_conn, ctx);
	struct nm_targ *t = &rc.targs[percpu_get(cpu_nr)];
	struct nm_msg msg; // dummy
	msg.targ = t;
	struct phttpd_global *pg = (struct phttpd_global *)
		t->g->garg_private;
	size_t msglen = pg->msglen;
        ssize_t len;
	int error, no_ok = 0;
	char *content = NULL;
	conn->written = 0;
	  tmp_cnt[percpu_get(cpu_nr)]++;

        len = ixev_recv(ctx, conn->response_buf, MAXQUERYLEN);
        if (len <= 0) {
                if (len != -EAGAIN) {
                        ixev_close(ctx);
                }
                return;
        }

	error = phttpd_req(conn->response_buf, len, &msg, &no_ok, &msglen, &content);
	if (unlikely(error)) {
	  ixev_close(ctx);
	  return;
	}
	
	if (!no_ok) {
		int httplen = pg->httplen;

		if (pg->http) {
			memcpy(conn->response_buf, pg->http, httplen);
		} else {
			httplen = generate_httphdr(msglen, conn->response_buf);
		}
		conn->httplen = httplen;
		if (content) {
			memcpy(conn->response_buf + httplen, content, msglen);
		} else {
		  if (httplen + pg->msglen > MAXQUERYLEN) {
		    fprintf(stderr, "message is too large and does not fit into the buffer!\n");
		    exit(1);
		  }
		  memcpy(conn->response_buf + httplen, data_buf, pg->msglen);
		}
#ifdef WITH_CLFLUSHOPT
		_mm_mfence();
		if (m->targ->g->emu_delay) {
			wait_ns(m->targ->g->emu_delay);
		}
#endif
		len = ixev_send(ctx, conn->response_buf + conn->written, httplen + msglen - conn->written);
		if (unlikely(len <= 0)) {
			perror("write");
			exit(1);
		} else {
		  conn->written += len;
		}
		if (httplen + msglen > conn->written) {
		  ixev_set_handler(ctx, IXEVOUT, &pp_stream_handler);
		}
	}

	return;
}

static struct ixev_ctx *pp_accept(struct ip_tuple *id)
{
        /* NOTE: we accept everything right now, did we want a port? */
        struct pp_conn *conn = mempool_alloc(&pp_conn_pool);
        if (!conn)
                return NULL;

	conn->response_buf = (char *)malloc(MAXQUERYLEN);
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
        .accept         = &pp_accept,
        .release        = &pp_release,
};

static void *pp_main(struct nm_targ *t)
{
        int ret;
	for(int i = 0; i < 48; i++) {
	  tmp_cnt[i] = 0;
	}
	

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

int reflex_init(void);

void *reflex_start(struct phttpd_global *pg, struct nm_garg *nmg, int **ret_fds, int *ret_fdnum) {
  int ret;
  unsigned int pp_conn_pool_entries;

  rc.fds = calloc(nmg->nthreads, sizeof(int));
  
  *ret_fds = rc.fds;
  *ret_fdnum = nmg->nthreads;
  
  reflex_init();

  pp_conn_pool_entries = 16 * 4096;
  pp_conn_pool_entries = ROUND_UP(pp_conn_pool_entries, MEMPOOL_DEFAULT_CHUNKSIZE);

  rc.pg = pg;
  ret = ixev_init(&pp_conn_ops);
  if (ret) {
    fprintf(stderr, "failed to initialize ixev\n");
    return NULL;
  }

  ret = mempool_create_datastore(&pp_conn_datastore, pp_conn_pool_entries, sizeof(struct pp_conn), "pp_conn");

  if (ret) {
    fprintf(stderr, "unable to create mempool\n");
    return NULL;
  }

  return (void *)&rc;
}

int reflex_setup_threads(struct nm_targ *targs, struct nm_garg *g, void *(*nm_thread)(void *)) {
  int ret, nr_cpu;
  int i;
  struct nm_targ *t;
	struct sigaction sa;
  
  rc.targs = targs;

  nr_cpu = cpus_active;
  if (nr_cpu < 1 || nr_cpu != g->nthreads) {
    fprintf(stderr, "got invalid cpu count %d\n", nr_cpu);
    return -1;
  }
  
  sys_spawnmode(true);

  for (i = 1; i < g->nthreads; i++) {
    t = &targs[i];
    ret = rte_eal_remote_launch(nm_thread, (void *)t, i);
    if (ret) {
      D("Unable to create thread %d: %s", i, strerror(errno));
      t->used = 0;
    }
  }
  
	sa.sa_handler = sigint_h;
	  if (sigaction(SIGINT, &sa, NULL) < 0) {
	    D("failed to install ^C handler: %s", strerror(errno));
	  }
  nm_thread(&targs[0]);
  
  return 0;
}

void reflex_cleanup(struct nm_garg *g) {
  struct reflex_ctx *ctx = (struct reflex_ctx *)g->be_ctx;
}

int reflex_setup_thread(struct nm_targ *t) {
  struct epoll_event ev;
  struct nm_garg *g = t->g;
  struct reflex_ctx *ctx = (struct reflex_ctx *)g->be_ctx;
  struct sockaddr_in sin;

  t->response_buf = (char *)malloc(MAXQUERYLEN);

  pp_main(t);
  return 0;
}

int reflex_handle_event(struct nm_targ *t) {
  return 0;
}

void reflex_cleanup_thread(struct nm_targ *t) {
}
