#ifndef ADDR_POOLS_H
#define ADDR_POOLS_H

#include "addr_pool.h"

/*----------------------------------------------------------------------------*/
typedef struct addr_pools *addr_pools_t;
/*----------------------------------------------------------------------------*/
/* CreateAddressPoolsPerCore()                                                 */
/* Create address pool only for the given core number.                        */
/* All addresses and port numbers should be in network order.                 */
/*----------------------------------------------------------------------------*/
addr_pools_t 
CreateAddressPoolsPerCore(int core, int num_target_queues, int num_queues, 
		in_addr_t saddr_base, int num_addr, in_addr_t daddr, in_port_t dport);
/*----------------------------------------------------------------------------*/
void
DestroyAddressPools(addr_pools_t aps);
/*----------------------------------------------------------------------------*/
int 
FetchAddressPerCoreFromPools(addr_pools_t aps, struct sockaddr_in *saddr);
/*----------------------------------------------------------------------------*/
int 
FreeAddressOnPools(addr_pools_t aps, const struct sockaddr_in *addr);
/*----------------------------------------------------------------------------*/

#endif /* ADDR_POOLS_H */
