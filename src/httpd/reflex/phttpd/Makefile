# Copyright 2013-16 Board of Trustees of Stanford University
# Copyright 2013-16 Ecole Polytechnique Federale Lausanne (EPFL)
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

# A Makefile for IX.
#
# path for this Makefile to work properly.

BASE    = /root/reflex/dp/

DPDK	= $(BASE)../deps/dpdk
SPDK    = $(BASE)../deps/spdk
DPDK_INC = -I$(DPDK)/build/include -I$(DPDK)/lib/librte_eal/common 
SPDK_INC = -idirafter$(SPDK)/include
INC	= -I$(BASE)../inc -I$(BASE)../inc/lwip -I$(BASE)../inc/lwip/ipv4 -I$(BASE)../inc/lwip/ipv6 $(DPDK_INC) $(SPDK_INC) -I$(BASE)../libix
CC	= gcc

DPDK_MACHINE_FLAGS = -march=native -DRTE_MACHINE_CPUFLAG_SSE -DRTE_MACHINE_CPUFLAG_SSE2 -DRTE_MACHINE_CPUFLAG_SSE3 -DRTE_MACHINE_CPUFLAG_SSSE3 -DRTE_MACHINE_CPUFLAG_SSE4_1 -DRTE_MACHINE_CPUFLAG_SSE4_2 -DRTE_MACHINE_CPUFLAG_AES -DRTE_MACHINE_CPUFLAG_PCLMULQDQ -DRTE_MACHINE_CPUFLAG_AVX -DRTE_MACHINE_CPUFLAG_RDRAND -DRTE_MACHINE_CPUFLAG_FSGSBASE -DRTE_MACHINE_CPUFLAG_F16C -DRTE_MACHINE_CPUFLAG_AVX2
CFLAGS	= -g -w -fno-dwarf2-cfi-asm -fno-asynchronous-unwind-tables -O3 -mno-red-zone $(INC) $(EXTRA_CFLAGS) -DSPDK_STRING_H $(DPDK_MACHINE_FLAGS)
PCIDMA	= $(BASE)../deps/pcidma
INC	+= -I$(PCIDMA)
LD	= gcc
LDFLAGS	= -T ix.ld
LDLIBS	= -lrt -lpthread -lm -lnuma -ldl -lconfig -lpciaccess


CFLAGS += -Werror -Wall -O2 -g -ggdb3
CFLAGS += -fgnu89-inline
CFLAGS += -DNOLIBNETMAP -DLIBNETMAP_NOTHREADSAFE
SPATH ?= -I../../sys/ -I../include -I../../libnetmap -DNETMAP_WITH_LIBS
CFLAGS +=$(SPATH)

LDFLAGS += -lpthread -lm
LDFLAGS += -lbsd

ifneq ($(DEBUG),)
CFLAGS += -DDEBUG
endif

ifneq ($(ENABLE_KSTATS),)
CFLAGS += -DENABLE_KSTATS
endif

SRCS = phttpd.c reflex.c
DIRS = core drivers lwip net

define register_dir
SRCS += $(patsubst %, $(1)/%, $(2))
endef

include $(patsubst %, %/dir.mk, $(DIRS))

%.generated.S: %.c
	$(CC) $(CFLAGS) -o $@ -S $<

all: ix

OBJS=$(subst .c,.o,$(SRCS))

depend: .depend

.depend: $(SRCS)
	bash -c "cat $(foreach SRC,$(SRCS),<($(CC) $(CFLAGS) -MM -MT $(SRC:.c=.o) $(SRC))) > ./.depend"

ifneq ($(MAKECMDGOALS),clean)
-include .depend
endif

DPDK_LIBS=
#DPDK_LIBS+=-Wl,-whole-archive $(DPDK)/build/lib/librte_pmd_ixgbe.a $(DPDK)/build/lib/librte_pmd_ena.a $(DPDK)/build/lib/librte_mempool_ring.a $(DPDK)/build/lib/librte_mempool.a -Wl,-no-whole-archive
DPDK_LIBS+=-Wl,-whole-archive $(DPDK)/build/lib/librte_pmd_i40e.a $(DPDK)/build/lib/librte_kvargs.a $(DPDK)/build/lib/librte_pmd_ena.a $(DPDK)/build/lib/librte_mempool_ring.a $(DPDK)/build/lib/librte_mempool.a -Wl,-no-whole-archive
DPDK_LIBS+=$(DPDK)/build/lib/librte_mbuf.a
DPDK_LIBS+=$(DPDK)/build/lib/librte_ethdev.a
DPDK_LIBS+=$(DPDK)/build/lib/librte_hash.a
DPDK_LIBS+=$(DPDK)/build/lib/librte_ring.a
DPDK_LIBS+=$(DPDK)/build/lib/librte_eal.a

SPDK_LIBS=$(SPDK)/build/lib/libspdk_nvme.a
SPDK_LIBS+=$(SPDK)/build/lib/libspdk_util.a
SPDK_LIBS+=$(SPDK)/build/lib/libspdk_env_dpdk.a
SPDK_LIBS+=$(SPDK)/build/lib/libspdk_log.a

IX_LIBS+=$(BASE)../libix/libix.a

nophttpd: $(DEPENDENCY) $(OBJS) ix.ld
	$(LD) $(LDFLAGS) -o nophttpd $(OBJS) $(SPDK_LIBS) $(DPDK_LIBS) $(LDLIBS) $(IX_LIBS)
clean:
	rm -f $(OBJS) nophttpd .depend

dist-clean: clean
	rm *~

