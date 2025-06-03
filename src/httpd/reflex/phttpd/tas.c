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

#include "tas.h"
#include "phttpd.h"

#define MAX_FLOW_NUM  (10000)
#define MAX_EVENTS (MAX_FLOW_NUM * 3)
#define TRACE_ERROR(str) fprintf(stderr, str)
#define PST_NAME	"pst:0"

#define DEBUG printf("debug: %s %d\n", __func__, __LINE__)

struct tas_ctx {
  int *fds;
};

static int inline
soopton(int fd, int level, int type)
{
	const int on = 1;

	if (setsockopt(fd, level, type, &on, sizeof(int)) < 0) {
		perror("setsockopt");
		return 1;
	}
	return 0;
}

static int inline
tas_do_setsockopt(int fd)
{
  int flag;
  if (soopton(fd, SOL_SOCKET, SO_REUSEADDR) ||
      soopton(fd, SOL_SOCKET, SO_REUSEPORT) /*||
					      soopton(fd, SOL_TCP, TCP_NODELAY)*/)
    return -EFAULT;
  if ((flag = fcntl(fd, F_GETFL, 0)) == -1) {
    return -1;
  }
  flag |= O_NONBLOCK;
  if (fcntl(fd, F_SETFL, flag) != 0) {
    return -1;
  }
  return 0;
}

void *tas_start(struct phttpd_global *pg, struct nm_garg *nmg, int **ret_fds, int *ret_fdnum) {
  struct tas_ctx *ctx = (struct tas_ctx *)malloc(sizeof(struct tas_ctx));

  ctx->fds = calloc(nmg->nthreads, sizeof(int));
  
  *ret_fds = ctx->fds;
  *ret_fdnum = nmg->nthreads;
  return (void *)ctx;
}

void tas_cleanup(struct nm_garg *g) {
  struct tas_ctx *ctx = (struct tas_ctx *)g->be_ctx;
  for (int i = 0; i < g->nthreads; i ++) {
    if (ctx->fds[i]) {
      close(ctx->fds[i]);
    }
  }
  free(ctx->fds);
}

int tas_setup_thread(struct nm_targ *t) {
  struct epoll_event ev;
  struct nm_garg *g = t->g;
  struct tas_ctx *ctx = (struct tas_ctx *)g->be_ctx;
  struct sockaddr_in sin;

  t->response_buf = (char *)malloc(MAXQUERYLEN);

  t->fd = epoll_create1(EPOLL_CLOEXEC);
  if (t->fd < 0) {
    perror("epoll_create1");
    t->cancel = 1;
    return -1;
  }
  
  int fd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) {
    perror("socket");
    return -1;
  }
  if (tas_do_setsockopt(fd)) {
    perror("setsockopt");
    return -1;
  }
  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = htonl(INADDR_ANY);
  sin.sin_port = htons(80);
  if (bind(fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
    perror("bind");
    return -1;
  }
  if (listen(fd, SOMAXCONN) != 0) {
    perror("listen");
    return -1;
  }
  
  /* XXX make variable ev num. */
  bzero(&ev, sizeof(ev));
  ev.events = POLLIN;
  ev.data.fd = fd;
  if (epoll_ctl(t->fd, EPOLL_CTL_ADD, ev.data.fd, &ev)) {
    perror("epoll_ctl");
    t->cancel = 1;
    return -1;
  }
  ctx->fds[t->me] = fd;
  return 0;
}

static int inline set_acceptedsock_opt(int fd) {
    int flag;
    if ((flag = fcntl(fd, F_GETFL, 0)) == -1) {
      return -1;
    }
    flag |= O_NONBLOCK;
    if (fcntl(fd, F_SETFL, flag) != 0) {
      return -1;
    }
    if (soopton(fd, SOL_TCP, TCP_NODELAY)) {
      return -EFAULT;
    }
    return 0;
}

static int tas_do_accept(struct nm_targ *t, int fd, int epfd)
{
  struct epoll_event ev;
  struct sockaddr_in sin;
  socklen_t addrlen = sizeof(sin);
  int newfd;
  while ((newfd = accept(fd, (struct sockaddr *)&sin, &addrlen)) != -1) {
    if (set_acceptedsock_opt(newfd)) {
      return -1;
    }
    
    if (newfd >= t->fdtable_siz) {
      close(newfd);
      return -1;
    }
    memset(&ev, 0, sizeof(ev));
    ev.events = POLLIN;
    ev.data.fd = newfd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, newfd, &ev);
  }
  return 0;
}

extern char *data_buf;


/* We assume GET/POST appears in the beginning of netmap buffer */
int
tas_phttpd_read(struct nm_msg *m)
{
  struct nm_targ *t = m->targ;
	struct phttpd_global *pg = (struct phttpd_global *)
		m->targ->g->garg_private;
	size_t msglen = pg->msglen;
	int len = 0;
	int error, no_ok = 0;
	char *content = NULL;
	int written = 0;

	len = read(m->fd, t->response_buf, MAXQUERYLEN);
	if (len <= 0) {
	  epoll_ctl(t->fd, EPOLL_CTL_DEL, m->fd, NULL);
		close(m->fd);
		return len == 0 ? 0 : -1;
	}

	error = phttpd_req(t->response_buf, len, m, &no_ok, &msglen, &content);
	if (unlikely(error))
	  return error;
	if (!no_ok) {
		int httplen = pg->httplen;

		if (pg->http) {
			memcpy(t->response_buf, pg->http, httplen);
		} else {
			httplen = generate_httphdr(msglen, t->response_buf);
		}
		if (content) {
			memcpy(t->response_buf + httplen, content, msglen);
		} else {
		  if (httplen + pg->msglen > MAXQUERYLEN) {
		    fprintf(stderr, "message is too large and does not fit into the buffer!\n");
		    exit(1);
		  }
		  memcpy(t->response_buf + httplen, data_buf, pg->msglen);
		}
#ifdef WITH_CLFLUSHOPT
		_mm_mfence();
		if (m->targ->g->emu_delay) {
			wait_ns(m->targ->g->emu_delay);
		}
#endif
		while (httplen + msglen > written) {
		len = write(m->fd, t->response_buf + written, httplen + msglen - written);
		if (unlikely(len <= 0)) {
			perror("write");
			exit(1);
		} else {
		  written += len;
		}
		}
	}
	return 0;
}

#define ARRAYSIZ(a)	(sizeof(a) / sizeof(a[0]))
int tas_handle_event(struct nm_targ *t) {
  struct nm_garg *g = t->g;
  struct tas_ctx *ctx = (struct tas_ctx *)g->be_ctx;
  struct nm_msg msg;
  int i, nfd, epfd = t->fd;
  int nevts = ARRAYSIZ(t->evts);
  
  struct epoll_event *evts = t->evts;

  nfd = epoll_wait(epfd, evts, nevts, g->polltimeo);
  if (nfd < 0) {
    perror("epoll_wait");
    return -1;
  }
  
  for (i = 0; i < nfd; i++) {
    int fd = evts[i].data.fd;

    if ((evts[i].events & EPOLLERR) ||
	(evts[i].events & EPOLLHUP) ||
	(!(evts[i].events & EPOLLIN))) {
      close(fd);
      continue;
    }

    if (fd == ctx->fds[t->me]) {
      tas_do_accept(t, fd, epfd);
    } else {
      msg.fd = fd;
      msg.targ = t;
      tas_phttpd_read(&msg);
    }
  }
  return 0;
}

void tas_cleanup_thread(struct nm_targ *t) {
}
