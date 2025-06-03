#ifndef _MTCP_H_
#define _MTCP_H_

struct nm_targ;
struct phttpd_global;
struct nm_targ;
struct nm_garg;
int mtcp_start(struct phttpd_global *pg, struct nm_garg *nmg);
void mtcp_cleanup();
int mtcp_setup_thread(struct nm_targ *t);
int mtcp_handle_event(struct nm_targ *t);
void mtcp_cleanup_thread(struct nm_targ *t);

#endif /* _MTCP_H_ */
