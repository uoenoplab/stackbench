#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <inttypes.h>
#include <sys/poll.h>
#ifdef __FreeBSD__
#include <sys/event.h>
#include <sys/stat.h>
#endif /* __FreeBSD__ */
#include <net/if.h>
#include <netinet/in.h>
#include <dirent.h>
#include <x86intrin.h>
#include <pthread.h>
#define NMLIB_EXTRA_SLOT 1
#include "nmlib.h"

#include <mtcp_api.h>
#include <mtcp_epoll.h>

#include "mtcp.h"
#include "phttpd.h"

#define MAX_FLOW_NUM  (10000)
#define MAX_EVENTS (MAX_FLOW_NUM * 3)
#define TRACE_ERROR(str) fprintf(stderr, str)
#define PST_NAME	"pst:0"
#define MAXQUERYLEN	32767

#define DEBUG printf("debug: %s %d\n", __func__, __LINE__)

pthread_mutex_t init_lock;
pthread_cond_t init_cond;
int noninitialized_cnt = 0;

#if 0
static int cancel = 0;
static void
SignalHandler(int signum)
{
	(void)signum;	/* UNUSED */
	cancel = 1;
}
#endif

// livaMEMO: TMP
static char tmp_buf[64];

int mtcp_start(struct phttpd_global *pg, struct nm_garg *nmg) {
  struct nm_garg *g;
  struct mtcp_conf mcfg;
  int *fds;
  int error = 0;
  int ret;
  mtcp_getconf(&mcfg);
  mcfg.num_cores = nmg->nthreads;
  mtcp_setconf(&mcfg);
  
  ret = mtcp_init("server.conf");
  if (ret) {
    fprintf(stderr, "failed to initialize mtcp");
    return -1;
  }

  //  mtcp_register_signal(SIGINT, SignalHandler);

  fds = calloc(nmg->nthreads, sizeof(int));

  for(int i = 0; i < 64; i++) {
    tmp_buf[i] = 'a';
  }

  noninitialized_cnt = nmg->nthreads;

  netmap_eventloop(PST_NAME, pg->ifname, (void **)&g, &error,
		   fds, 1, NULL, 0, nmg, pg);
  return 0;
}

void mtcp_cleanup() {
  mtcp_destroy();
}

static int InitializeServerThread(struct nm_targ *t) {
  /* create mtcp context: this will spawn an mtcp thread */
  mtcp_core_affinitize(t->me);
  t->mctx = mtcp_create_context(t->me);
  if (!t->mctx) {
    fprintf(stderr, "failed to mtcp_create_context");
    return -1;
  }

  /* wait until all threads are ready */
  pthread_mutex_lock(&init_lock);
  noninitialized_cnt--;
  if (noninitialized_cnt != 0) {
    pthread_cond_wait(&init_cond, &init_lock);
  } else {
    pthread_cond_broadcast(&init_cond);
  }
  pthread_mutex_unlock(&init_lock);
  
  t->fd = mtcp_epoll_create(t->mctx, MAX_EVENTS);
  if (t->fd < 0) {
    fprintf(stderr, "failed to create epoll descriptor");
    mtcp_destroy_context(t->mctx);
    t->cancel = 1;
    return -1;
  }

  return 0;
}

static int CreateListeningSocket(struct nm_targ *t) {
  int listener;
  struct mtcp_epoll_event ev;
  struct sockaddr_in saddr;
  int ret;
  int backlog = 4096;
  
  /* create socket and set it as nonblocking */
  listener = mtcp_socket(t->mctx, AF_INET, SOCK_STREAM, 0);
  if (listener < 0) {
    TRACE_ERROR("Failed to create listening socket!\n");
    return -1;
  }
  t->g->fds[t->me] = listener;
  ret = mtcp_setsock_nonblock(t->mctx, listener);
  if (ret < 0) {
    TRACE_ERROR("Failed to set socket in nonblocking mode.\n");
    return -1;
  }

  /* bind to port 80 */
  saddr.sin_family = AF_INET;
  saddr.sin_addr.s_addr = INADDR_ANY;
  saddr.sin_port = htons(80);
  ret = mtcp_bind(t->mctx, listener, 
		  (struct sockaddr *)&saddr, sizeof(struct sockaddr_in));
  if (ret < 0) {
    TRACE_ERROR("Failed to bind to the listening socket!\n");
    return -1;
  }
  
  /* listen (backlog: can be configured) */
  ret = mtcp_listen(t->mctx, listener, backlog);
  if (ret < 0) {
    TRACE_ERROR("mtcp_listen() failed!\n");
    return -1;
  }

  
  /* XXX make variable ev num. */
  ev.events = MTCP_EPOLLIN;
  ev.data.sockid = listener;
  if (mtcp_epoll_ctl(t->mctx, t->fd, MTCP_EPOLL_CTL_ADD, listener, &ev)) {
    perror("epoll_ctl");
    return -1;
  }
  return 0;
}

int mtcp_setup_thread(struct nm_targ *t) {
  int ret;
  ret = InitializeServerThread(t);
  if (ret != 0) {
    return ret;
  }
  ret = CreateListeningSocket(t);
  if (ret != 0) {
    return ret;
  }
  
  return 0;
}

/* We assume GET/POST appears in the beginning of netmap buffer */
static int
mtcp_httpd_read(struct nm_msg *m)
{
  struct nm_targ *t = m->targ;
  struct phttpd_global *pg = (struct phttpd_global *)
    m->targ->g->garg_private;
  size_t msglen = pg->msglen, len = 0;
  int error, no_ok = 0;
  char *content = NULL;
  char buf[MAXQUERYLEN];
  
  len = mtcp_read(t->mctx, m->fd, buf, sizeof(buf));
  if (len <= 0) {
    mtcp_epoll_ctl(t->mctx, t->fd, MTCP_EPOLL_CTL_DEL, m->fd, NULL);
    mtcp_close(t->mctx, m->fd);
    return len == 0 ? 0 : -1;
  }
  
  error = phttpd_req(buf, len, m, &no_ok, &msglen, &content);
  if (unlikely(error))
    return error;
  if (!no_ok) {
    int httplen = pg->httplen;
    
    if (pg->http) {
      memcpy(buf, pg->http, httplen);
    } else {
      httplen = generate_httphdr(msglen, buf);
    }
    if (content) {
      memcpy(buf + httplen, content, msglen);
    } else {
      // livaMEMO: TMP
      memcpy(buf + httplen, tmp_buf, 64);
    }
#ifdef WITH_CLFLUSHOPT
    _mm_mfence();
    if (m->targ->g->emu_delay) {
      wait_ns(m->targ->g->emu_delay);
    }
#endif
    len = mtcp_write(t->mctx, m->fd, buf, httplen + msglen);
    if (unlikely(len < 0)) {
      perror("write");
    } else if (unlikely(len < httplen + msglen)) {
      RD(1, "written %ld len %ld", len, httplen + msglen);
    }
  }
  return 0;
}


static int mtcp_do_accept(struct nm_targ *t, int listener, int epfd)
{
  struct mtcp_epoll_event ev;
  int newfd;
  //int val = 1;
  // MEMO: refer to AcceptConnection
  while ((newfd = mtcp_accept(t->mctx, listener, NULL, NULL)) != -1) {
#if 0
    // from nophttpd
    // equivalent part in epserver: not yet found
    if (ioctl(fd, FIONBIO, &(int){1}) < 0) {
      perror("ioctl");
    }
#endif
#if 0
    // from nophttpd
    // equivalent part in epserver: possibly, mtcp_setsock_nonblock()
    int yes = 1;
    setsockopt(newfd, SOL_SOCKET, SO_BUSY_POLL, &yes, sizeof(yes));
#endif
    if (newfd >= MAX_FLOW_NUM) {
      fprintf(stderr, "invalid socket id\n");
      break;
    }
    //memset(&ev, 0, sizeof(ev));
    ev.events = MTCP_EPOLLIN;
    ev.data.sockid = newfd;
    mtcp_setsock_nonblock(t->mctx, newfd);
    mtcp_epoll_ctl(t->mctx, epfd, MTCP_EPOLL_CTL_ADD, newfd, &ev);
  }
  return 0;
}

// MEMO: refer to the end of RunServerThread
int mtcp_handle_event(struct nm_targ *t) {
  struct nm_garg *g = t->g;
  struct nm_msg msg;
  int i, nfd, epfd = t->fd;
  int nevts = ARRAYSIZ(t->evts);
  struct mtcp_epoll_event *evts = t->mtcp_evts;

  nfd = mtcp_epoll_wait(t->mctx, epfd, evts, nevts, g->polltimeo);
  if (nfd < 0) {
    fprintf(stderr, "epoll_wait");
    return -1;
  }
  for (i = 0; i < nfd; i++) {
    int fd = evts[i].data.sockid;
    int listener = t->g->fds[t->me];

    if (fd == listener) {
      mtcp_do_accept(t, fd, epfd);
    } else {
      msg.fd = fd;
      msg.targ = t;
      mtcp_httpd_read(&msg);
    }
  }
  return 0;
}

void mtcp_cleanup_thread(struct nm_targ *t) {
    mtcp_destroy_context(t->mctx);
}

// from epserver:
// equivalent part in nophttpd: process each events[i].events in RunServerThread
// ** needs implementation **

