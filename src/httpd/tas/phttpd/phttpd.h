#ifndef _PHTTPD_H_
#define _PHTTPD_H_

#include<net/netmap.h>
#include<net/netmap_user.h>
#include<net/netmap_paste.h>
#include<ctrs.h>
#include <x86intrin.h>
#ifdef __cplusplus
extern "C" {
#include <libnetmap.h>
}
#else
#include <libnetmap.h>
#endif /* __cplusplus */
#define NMLIB_EXTRA_SLOT 1
#define EPOLLEVENTS 2048

struct nm_msg {
	struct netmap_ring *rxring;
	struct netmap_ring *txring;
	struct netmap_slot *slot;
	struct nm_targ *targ;
	int fd;
};

struct nm_garg {
	char ifname[NETMAP_REQ_IFNAMSIZ*2]; // must be here
	struct nmport_d *nmd;
	void *(*td_body)(void *);
	int nthreads;
	int affinity;
	int dev_type;
	int td_type;
	int main_fd;
	int system_cpus;
	int cpus;
	uint32_t extra_bufs;		/* goes in nr_arg3 */
	uint64_t extmem_siz;
	u_int ring_objsize;
	int extra_pipes;	/* goes in nr_arg1 */
	char *nmr_config;
	char *extmem;		/* goes to nr_arg1+ */
#define	STATS_WIN	15
	int win_idx;
	int64_t win[STATS_WIN];
	int wait_link;
	int polltimeo;
	int pollevents;
#ifdef __FreeBSD__
	struct timespec *polltimeo_ts;
#endif
	int verbose;
	int report_interval;
#define OPT_PPS_STATS   2048
	int options;
	int targ_opaque_len; // passed down to targ

	struct nmreq_header nm_hdr; // cache decoded
	int (*data)(struct nm_msg *);
	void (*connection)(struct nm_msg *);
	int (*read)(struct nm_msg *);
	int (*thread)(struct nm_targ *);
	int (*writable)(struct nm_msg *);
	int *fds;
	u_int fdnum;
	int *cfds;
	u_int cfdnum;
	int emu_delay;
	void *garg_private;
        void *be_ctx;
	char ifname2[NETMAP_REQ_IFNAMSIZ];
};

struct nm_targ {
	struct nm_garg *g;
	struct nmport_d *nmd;
	/* these ought to be volatile, but they are
	 * only sampled and errors should not accumulate
	 */
	struct my_ctrs ctr;

	struct timespec tic, toc;
	int used;
	int completed;
	int cancel;
	int fd;
	int me;
	int affinity;
	pthread_t thread;
#ifdef NMLIB_EXTRA_SLOT
	struct netmap_slot *extra;
#else
	uint32_t *extra;
#endif
	uint32_t extra_cur;
	uint32_t extra_num;
	int *fdtable;
	int fdtable_siz;
#ifdef linux
	struct epoll_event evts[EPOLLEVENTS];
#else
	struct kevent	evts[EPOLLEVENTS];
#endif /* linux */
	void *opaque;
	char *response_buf;
};

#ifdef WITH_NOFLUSH
#define _mm_clflush(p) (void)(p)
#define _mm_mfence() (0)
#endif
#ifdef WITH_CLFLUSHOPT
#define _mm_clflush(p) _mm_clflushopt(p)
#endif

int
phttpd_req(char *req, int len, struct nm_msg *m, int *no_ok,
	   size_t *msglen, char **content);
ssize_t
generate_httphdr(size_t content_length, char *buf);

struct phttpd_global {
  char ifname[NETMAP_REQ_IFNAMSIZ];
  int extmemfd;
  int sd;
  char *http;
  int httplen;
  int msglen;
  struct {
    int	flags;
    size_t	size;
    char	*dir; // directory path for data, metadata ane ppool
  } dba;
};

//#define MAXQUERYLEN	32767 // original value
#define MAXQUERYLEN	2099200

#endif /* _PHTTPD_H_ */
