#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include "addr_pools.h"
#include "rss.h"
#include "debug.h"

struct addr_list;
/*----------------------------------------------------------------------------*/
struct addr_entry2
{
	struct sockaddr_in addr;
        struct addr_list *al;
	TAILQ_ENTRY(addr_entry2) addr_link;
};
/*----------------------------------------------------------------------------*/
struct addr_list
{
	int num_free;
	int num_used;

	pthread_mutex_t lock;
	TAILQ_HEAD(, addr_entry2) free_list;
	TAILQ_HEAD(, addr_entry2) used_list;
};
/*----------------------------------------------------------------------------*/
struct addr_map2
{
	struct addr_entry2 *addrmap[MAX_PORT];
};
/*----------------------------------------------------------------------------*/
struct addr_pools
{
	struct addr_entry2 *pool;		/* address pool */
        struct addr_list *lists;
	struct addr_map2 *mapper;		/* address map  */

	uint32_t addr_base;				/* in host order */
	int num_addr;					/* number of addresses in use */
	int num_entry;
        int num_target_queues;
        volatile int target_queue_cnt;
};
/*----------------------------------------------------------------------------*/
addr_pools_t 
CreateAddressPoolsPerCore(int core, int num_target_queues, int num_queues, 
		in_addr_t saddr_base, int num_addr, in_addr_t daddr, in_port_t dport)
{
	struct addr_pools *aps;
	struct addr_list *al;
	/*int num_entry;*/
	int i, j, cnt;
	in_addr_t saddr;
	uint32_t saddr_h, daddr_h;
	uint16_t sport_h, dport_h;
	int rss_core, rss_target_core;
	uint8_t endian_check = FetchEndianType();

	aps = (addr_pools_t)calloc(1, sizeof(struct addr_pools));
	if (!aps)
	  return NULL;

	/* initialize address pool */
	/*num_entry = (num_addr * (MAX_PORT - MIN_PORT)) / num_queues;*/
	aps->pool = (struct addr_entry2 *)calloc(num_addr * (MAX_PORT - MIN_PORT)/*num_entry*/, sizeof(struct addr_entry2));

	if (!aps->pool) {
	  goto error;
	}

	/* initialize address map */
	aps->mapper = (struct addr_map2 *)calloc(num_addr, sizeof(struct addr_map2));
	if (!aps->mapper) {
	  goto error;
	}

	if (num_target_queues <= 0) {
	  num_target_queues = 1;
	}

	aps->lists = (struct addr_list *)calloc(num_target_queues, sizeof(struct addr_list));
	for (i = 0; i < num_target_queues; i++) {
	  al = &aps->lists[i];
	  
	  TAILQ_INIT(&al->free_list);
	  TAILQ_INIT(&al->used_list);

	  if (pthread_mutex_init(&al->lock, NULL)) {
	    goto error;
	  }
	  
	  pthread_mutex_lock(&al->lock);
	}
	

	aps->addr_base = ntohl(saddr_base);
	aps->num_addr = num_addr;
	aps->num_target_queues = num_target_queues;
	aps->target_queue_cnt = core;
	if (num_target_queues > num_queues) {
	  aps->target_queue_cnt = (int)(aps->target_queue_cnt * ((num_target_queues + 0.0) / num_queues));
	}
	printf("DEBUG: CreateAddressPoolsPerCore: %d %d %d\n", core, aps->target_queue_cnt, num_target_queues);
	daddr_h = ntohl(daddr);
	dport_h = ntohs(dport);

	/* search address space to get RSS-friendly addresses */
	cnt = 0;
	for (i = 0; i < num_addr; i++) {
		saddr_h = aps->addr_base + i;
		saddr = htonl(saddr_h);
		for (j = MIN_PORT; j < MAX_PORT; j++) {
		  /*if (cnt >= num_entry)*/
		  /*		break;   */

			sport_h = j;
			rss_core = GetRSSCPUCore(daddr_h, saddr_h, dport_h, sport_h, num_queues, endian_check);
			if (rss_core != core)
				continue;

			rss_target_core = GetRSSCPUCore(daddr_h, saddr_h, dport_h, sport_h, num_target_queues, endian_check);
			al = &aps->lists[rss_target_core];

			aps->pool[cnt].addr.sin_addr.s_addr = saddr;
			aps->pool[cnt].addr.sin_port = htons(sport_h);
			aps->pool[cnt].al = al;
			aps->mapper[i].addrmap[j] = &aps->pool[cnt];
			TAILQ_INSERT_TAIL(&al->free_list, &aps->pool[cnt], addr_link);
			cnt++;
			al->num_free++;
		}
	}

	aps->num_entry = cnt;
	//fprintf(stderr, "CPU %d: Created %d address entries.\n", core, cnt);
	if (aps->num_entry < CONFIG.max_concurrency) {
		fprintf(stderr, "[WARINING] Available # addresses (%d) is smaller than"
				" the max concurrency (%d).\n", 
				aps->num_entry, CONFIG.max_concurrency);
	}
	
	printf("DEBUG %2d:", core);
	for (i = 0; i < num_target_queues; i++) {
	  struct addr_list *al = &aps->lists[i];
	  al->num_used = 0;
	  printf(" %d:%4d", i, al->num_free);
	  pthread_mutex_unlock(&al->lock);
	}
	printf("\n");

	return aps;
 error:
	if (aps->mapper) {
	  free(aps->mapper);
	}
	if (aps->lists) {
	  free(aps->lists);
	}
	if (aps->pool) {
	  free(aps->pool);
	}
	free(aps);
	return NULL;	
}
/*----------------------------------------------------------------------------*/
void
DestroyAddressPools(addr_pools_t aps)
{
	if (!aps)
		return;

	if (aps->pool) {
		free(aps->pool);
		aps->pool = NULL;
	}

	if (aps->mapper) {
		free(aps->mapper);
		aps->mapper = NULL;
	}
	if (aps->lists) {
	  for (int i = 0; i < aps->num_target_queues; i++) {
	    struct addr_list *al = &aps->lists[i];
	    pthread_mutex_destroy(&al->lock);
	  }
	  free(aps->lists);
	  aps->lists = NULL;
	}

	free(aps);
}
/*----------------------------------------------------------------------------*/
int 
FetchAddressPerCoreFromPools(addr_pools_t aps, struct sockaddr_in *saddr)
{
	struct addr_entry2 *walk;
	int list_index;
	struct addr_list *al;
	int prev_target_queue_cnt = aps->target_queue_cnt;

	if (!aps || !saddr)
		return -1;

	while(aps->target_queue_cnt - prev_target_queue_cnt < aps->num_target_queues) {
	  /* increment counter to ensure target-side distribution */
	  list_index = __sync_fetch_and_add(&aps->target_queue_cnt, 1) % aps->num_target_queues;
	  al = &aps->lists[list_index];
	
	  pthread_mutex_lock(&al->lock);
	
	  /* we don't need to calculate RSSCPUCore if mtcp_init_rss is called */
	  walk = TAILQ_FIRST(&al->free_list);
	  if (walk) {
	    *saddr = walk->addr;
	    TAILQ_REMOVE(&al->free_list, walk, addr_link);
	    TAILQ_INSERT_TAIL(&al->used_list, walk, addr_link);
	    al->num_free--;
	    al->num_used++;
	  }
	
	  pthread_mutex_unlock(&al->lock);

	  if (walk) {
	    return 0;
	  }
	}
	/* checked (almost) all queues, but nothing found */
	return -1;
}
/*----------------------------------------------------------------------------*/
int 
FreeAddressOnPools(addr_pools_t aps, const struct sockaddr_in *addr)
{
	struct addr_entry2 *walk;
	struct addr_list *al;
	int ret = -1;

	if (!aps || !addr)
		return -1;

	uint32_t addr_h = ntohl(addr->sin_addr.s_addr);
	uint16_t port_h = ntohs(addr->sin_port);
	int index = addr_h - aps->addr_base;
	
	if (index >= 0 && index < aps->num_addr) {
	  walk = aps->mapper[addr_h - aps->addr_base].addrmap[port_h];
	} else {
	  walk = NULL;
	}

	if (walk) {
	  al = walk->al;
	  pthread_mutex_lock(&al->lock);
	  TAILQ_REMOVE(&al->used_list, walk, addr_link);
	  TAILQ_INSERT_TAIL(&al->free_list, walk, addr_link);
	  al->num_free++;
	  al->num_used--;
	  ret = 0;
	  pthread_mutex_unlock(&al->lock);
	}

	return ret;
}
/*----------------------------------------------------------------------------*/
