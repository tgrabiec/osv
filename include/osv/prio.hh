/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef PRIO_HH_
#define  PRIO_HH_

enum class init_prio : int {
    console = 101,
    sort,
    cpus,
    fpranges,
    pt_root,
    mempool,
    pagecache,
    threadlist,
    pthread,
    notifiers,
    acpi,
    apic,
    vma_list,
    reclaimer,
    sched,
    clock,
    hpet,
    tracepoint_base,
    malloc_pools,
    idt,
};

#endif
