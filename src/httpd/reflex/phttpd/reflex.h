#ifndef _REFLEX_H_
#define _REFLEX_H_

struct nm_targ;
struct phttpd_global;
struct nm_targ;
struct nm_garg;
void *reflex_start(struct phttpd_global *pg, struct nm_garg *nmg, int **ret_fds, int *ret_fdnum);
void reflex_cleanup(struct nm_garg *g);
int reflex_setup_threads(struct nm_targ *targs, struct nm_garg *g, void *(*nm_thread)(void *));
int reflex_setup_thread(struct nm_targ *t);
int reflex_handle_event(struct nm_targ *t);
void reflex_cleanup_thread(struct nm_targ *t);

#endif /* _REFLEX_H_ */
