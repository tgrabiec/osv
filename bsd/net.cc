/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/debug.hh>
#include <sys/time.h>
#include <osv/mempool.hh>

#include <bsd/porting/callout.h>
#include <bsd/porting/netport.h>
#include <bsd/porting/networking.hh>
#include <bsd/porting/route.h>

#include <bsd/sys/sys/libkern.h>
#include <bsd/sys/sys/eventhandler.h>
#include <bsd/sys/sys/mbuf.h>
#include <bsd/sys/sys/domain.h>
#include <bsd/sys/net/netisr.h>
#include <bsd/sys/net/if.h>
#include <bsd/sys/net/if_llatbl.h>
#include <bsd/sys/net/pfil.h>
#include <bsd/sys/netinet/igmp.h>
#include <bsd/sys/netinet/if_ether.h>
#include <bsd/sys/netinet/in_pcb.h>
#include <bsd/sys/netinet/cc.h>
#include <bsd/sys/net/ethernet.h>
#include <bsd/sys/net/route.h>
#include <machine/param.h>

/* Generation of ip ids */
void ip_initid(void);

extern "C" {

    /* AF_INET */
    extern  struct domain inetdomain;
    /* AF_ROUTE */
    extern  struct domain routedomain;

    extern void system_taskq_init(void *arg);
    extern void opensolaris_load(void *arg);
    extern void callb_init(void *arg);

    extern void zfs_init(void *arg);

    // taskqueue
    #include <bsd/sys/sys/taskqueue.h>
    #include <bsd/sys/sys/priority.h>
    TASKQUEUE_DEFINE_THREAD(thread);
}

static void physmem_init()
{
    physmem = memory::phys_mem_size / memory::page_size;
}

void net_init(void)
{
    debug("net: initializing");

    physmem_init();

    // main taskqueue
    taskqueue_define_thread(NULL);

    // Initialize callouts
    init_callouts();

    /* Random */
    struct timeval tv;
    bsd_srandom(tv.tv_sec ^ tv.tv_usec);
    ip_initid();

    tunable_mbinit(NULL);
    init_maxsockets(NULL);
    arc4_init();
    eventhandler_init(NULL);
    opensolaris_load(NULL);
    mbuf_init(NULL);
    netisr_init(NULL);
    if_init(NULL);
    vnet_if_init(NULL);
    ether_init(NULL);
    callb_init(NULL);
    system_taskq_init(NULL);
    vnet_lltable_init();
    igmp_init();
    vnet_igmp_init();
    vnet_pfil_init();
    domaininit(NULL);
    OSV_DOMAIN_SET(inet);
    OSV_DOMAIN_SET(route);
    rts_init();
    route_init();
    vnet_route_init();
    ipport_tick_init(NULL);
    arp_init();
    domainfinalize(NULL);
    cc_init();
    if_attachdomain(NULL);
    vnet_loif_init();

    /* Start the loopback device */
    osv::start_if("lo0", "127.0.0.1", "255.0.0.0");
    osv::ifup("lo0");
    zfs_init(NULL);

    debug(" - done\n");
}
