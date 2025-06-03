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
#define NMLIB_EXTRA_SLOT 1
#include "nmlib.h"
#include "phttpd.h"

#include "ff_config.h"
#include "ff_api.h"
#include "ff_epoll.h"

#define PST_NAME	"pst:0"

#define MAX_EVENTS 2048

struct epoll_event ev;
struct epoll_event events[MAX_EVENTS];

int epfd;
int sockfd;

#define FFMUTEX "/ffmutex"
#define FFCOND  "/ffcond"
#define FFCNT  "/ffcnt"
pthread_mutex_t *init_lock;
pthread_cond_t *init_cond;
int *noninitialized_cnt;

extern char *data_buf;

static void *create_shm(const char *name, size_t size) {
  void *mem;
  int id = shm_open(name, O_CREAT | O_RDWR | O_TRUNC, S_IRWXU | S_IRWXG);
  if (id < 0) {
    perror("shm_open failed\n");
    return NULL;
  }
  if (ftruncate(id, size) == -1) {
    perror("ftruncate failed\n");
    return NULL;
  }
  mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, id, 0);
  if (mem == MAP_FAILED) {
    perror("mmap failed");
    return NULL;
  }
  return mem;
}

static int fstack_init_semaphore() {
  pthread_mutexattr_t mattr;
  pthread_condattr_t cattr;

  init_lock = (pthread_mutex_t *)create_shm(FFMUTEX, sizeof(pthread_mutex_t));
  if (!init_lock) {
    return -1;
  }
  init_cond = (pthread_cond_t *)create_shm(FFCOND, sizeof(pthread_cond_t));
  if (!init_cond) {
    return -1;
  }

  pthread_mutexattr_init(&mattr);
  pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
  if (pthread_mutex_init(init_lock, &mattr) < 0) {
    printf("pthread_mutex_init failed");
    return -1;
  }
  pthread_mutexattr_destroy(&mattr);

  pthread_condattr_init(&cattr);
  pthread_condattr_setpshared(&cattr, PTHREAD_PROCESS_SHARED);
  if (pthread_cond_init(init_cond, &cattr) < 0) {
    printf("pthread_cond_init failed");
    return -1;
  }
  pthread_condattr_destroy(&cattr);

  noninitialized_cnt = (int *)create_shm(FFCNT, sizeof(int));
  if (!noninitialized_cnt) {
    return -1;
  }

  return 0;
}

int fstack_start(struct phttpd_global *pg, struct nm_garg *nmg) {
  struct nm_garg *g;
  int error = 0;
  pid_t pid;
  int id;

  int procs = nmg->nthreads;
  nmg->nthreads = 1;

  if (fstack_init_semaphore() < 0) {
    return -1;
  }
  *noninitialized_cnt = procs;

  for(id = 0; id < procs - 1; id++) {
    if ((pid = fork()) < 0) {
      printf("fork faild\n");
      return -1;
    }
    if (pid == 0) { /* child */
      break;
    }
  }

  if (pid != 0) { /* primary */
    if (id != procs - 1) {
      printf("???\n");
      return -1;
    }
    char *argv[] = {
		    "nophttpd",
		    "--proc-type=primary",
		    "--proc-id=0",
    };
    ff_init(3, argv);
  } else {
    char procid[32];
    snprintf(procid, sizeof(procid), "--proc-id=%d", id + 1);
    char *argv[] = {
		    "nophttpd",
		    "--proc-type=secondary",
		    procid,
    };
    sleep(3);  // wait until primary starts
    ff_init(3, argv);
  }

  sockfd = ff_socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    printf("ff_socket failed\n");
    return -1;
  }

  int on = 1;
  ff_ioctl(sockfd, FIONBIO, &on);

  struct sockaddr_in my_addr;
  bzero(&my_addr, sizeof(my_addr));
  my_addr.sin_family = AF_INET;
  my_addr.sin_port = htons(80);
  my_addr.sin_addr.s_addr = htonl(INADDR_ANY);

  int ret = ff_bind(sockfd, (struct linux_sockaddr *)&my_addr, sizeof(my_addr));
  if (ret < 0) {
    printf("ff_bind failed\n");
    return -1;
  }

  /* wait until all threads are ready */
  pthread_mutex_lock(init_lock);
  (*noninitialized_cnt)--;
  if (*noninitialized_cnt != 0) {
    pthread_cond_wait(init_cond, init_lock);
  } else {
    pthread_cond_broadcast(init_cond);
  }
  pthread_mutex_unlock(init_lock);

  ret = ff_listen(sockfd, MAX_EVENTS);
  if (ret < 0) {
    printf("ff_listen failed\n");
    return -1;
  }

  epfd = ff_epoll_create(0);
  if (epfd <= 0) {
    return -1;
  }
  ev.data.fd = sockfd;
  ev.events = EPOLLIN;
  ff_epoll_ctl(epfd, EPOLL_CTL_ADD, sockfd, &ev);

  netmap_eventloop(PST_NAME, pg->ifname, (void **)&g, &error,
		   &sockfd, 1, NULL, 0, nmg, pg);
  return 0;
}

void fstack_cleanup() {
}

static int
fstack_httpd_read(struct nm_msg *m)
{
  struct nm_targ *t = m->targ;
  struct phttpd_global *pg = (struct phttpd_global *)t->g->garg_private;
  size_t msglen = pg->msglen;
  int len = 0;
  int error, no_ok = 0;
  char *content = NULL;
  int written = 0;

  len = ff_read(m->fd, t->response_buf, MAXQUERYLEN);
  if (len <= 0) {
    ff_epoll_ctl(epfd, EPOLL_CTL_DEL, m->fd, NULL);
    ff_close(m->fd);
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
      memcpy(t->response_buf + httplen, data_buf, pg->msglen);
    }
#ifdef WITH_CLFLUSHOPT
    _mm_mfence();
    if (g->emu_delay) {
      wait_ns(g->emu_delay);
    }
#endif
    while (httplen + msglen > written) {
      len = ff_write(m->fd, t->response_buf + written, httplen + msglen - written);
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

static int fstack_handle_event(struct nm_targ *t) {
  struct nm_garg *g = t->g;
  struct nm_msg msg;
  
    int nevents = ff_epoll_wait(epfd,  events, MAX_EVENTS, g->polltimeo);
    int i;

    for (i = 0; i < nevents; ++i) {
        /* Handle new connect */
      int fd = events[i].data.fd;
        if (fd == sockfd) {
            while (1) {
                int nclientfd = ff_accept(sockfd, NULL, NULL);
                if (nclientfd < 0) {
                    break;
                }

                /* Add to event list */
                ev.data.fd = nclientfd;
                ev.events  = EPOLLIN;
                if (ff_epoll_ctl(epfd, EPOLL_CTL_ADD, nclientfd, &ev) != 0) {
                    printf("ff_epoll_ctl failed:%d, %s\n", errno,
                        strerror(errno));
                    break;
                }
            }
        } else {
            if (events[i].events & EPOLLERR ) {
                /* Simply close socket */
                ff_epoll_ctl(epfd, EPOLL_CTL_DEL,  events[i].data.fd, NULL);
                ff_close(events[i].data.fd);
            } else if (events[i].events & EPOLLIN) {
	      msg.fd = fd;
	      msg.targ = t;
	      fstack_httpd_read(&msg);
            } else {
                printf("unknown event: %8.8X\n", events[i].events);
            }
        }
    }  
  return 0;
}

int fstack_create_thread(struct nm_targ *t) {
  t->response_buf = (char *)malloc(MAXQUERYLEN);
  ff_run((int (*)(void*))fstack_handle_event, t);
  return 0;
}

void fstack_cleanup_thread(struct nm_targ *t) {
}
