#ifndef _FSTACK_H_
#define _FSTACK_H_

struct nm_targ;
struct phttpd_global;
struct nm_targ;
struct nm_garg;
int fstack_start(struct phttpd_global *pg, struct nm_garg *nmg);
void fstack_cleanup();
int fstack_create_thread(struct nm_targ *t);
void fstack_cleanup_thread(struct nm_targ *t);

#endif /* _FSTACK_H_ */
