#ifndef _PHTTPD_H_
#define _PHTTPD_H_
#include <x86intrin.h>
#define NMLIB_EXTRA_SLOT 1
#include "nmlib.h"
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
