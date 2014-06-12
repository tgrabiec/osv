/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/types.h>
#include <osv/pvclock-abi.hh>
#include "processor.hh"
#include <osv/barrier.hh>
#include <osv/irqlock.hh>
#include <osv/mutex.h>
#include <osv/debug.hh>
#include <osv/percpu.hh>
#include <assert.h>

namespace pvclock {

u64 wall_clock_boot(pvclock_wall_clock *_wall)
{
    u32 v1, v2;
    u64 w;
    do {
        v1 = _wall->version;
        barrier();
        w = u64(_wall->sec) * 1000000000 + _wall->nsec;
        barrier();
        v2 = _wall->version;
    } while (v1 != v2);
    return w;
}

static PERCPU(u64, last_time);
static PERCPU(u64, last_tsc);

u64 system_time(pvclock_vcpu_time_info *sys)
{
    u32 v1, v2;
    u64 time;
    irq_save_lock_type irqlock;
    SCOPE_LOCK(irqlock);
    do {
        v1 = sys->version;
        barrier();
        processor::lfence();
        u64 tsc = processor::rdtsc();
        assert(tsc >= *last_tsc);
        *last_tsc = tsc;
        time = sys->system_time + processor_to_nano(sys, tsc - sys->tsc_timestamp);
        barrier();
        v2 = sys->version;
    } while ((v1 & 1) || v1 != v2);
    assert(time >= *last_time);
    *last_time = time;
    return time;
}
};
