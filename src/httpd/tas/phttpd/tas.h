#ifndef _TAS_H_
#define _TAS_H_

struct nm_targ;
struct phttpd_global;
struct nm_targ;
struct nm_garg;
void *tas_start(struct phttpd_global *pg, struct nm_garg *nmg, int **ret_fds, int *ret_fdnum);
void tas_cleanup(struct nm_garg *g);
int tas_setup_thread(struct nm_targ *t);
int tas_handle_event(struct nm_targ *t);
void tas_cleanup_thread(struct nm_targ *t);

#endif /* _TAS_H_ */
