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
#include <osv/trace.hh>
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

inline u64 processor_to_nano2(pvclock_transformation_params& params, u64 time)
{
    if (params.tsc_shift >= 0) {
        time <<= params.tsc_shift;
    } else {
        time >>= -params.tsc_shift;
    }
    asm("mul %1; shrd $32, %%rdx, %0"
            : "+a"(time)
            : "rm"(u64(params.tsc_to_system_mul))
            : "rdx");
    return time;
}

static inline
u64 transform(pvclock_transformation_params& params, u64 tsc)
{
    return params.system_time + processor_to_nano2(params, tsc - params.tsc_timestamp);
}

template<typename Func, typename Result = u64>
static inline
Result read_atomic(pvclock_vcpu_time_info* info, Func func)
{
    u32 v1, v2;
    Result result;
    do {
        v1 = info->version;
        barrier();
        result = func(info);
        barrier();
        v2 = info->version;
    } while ((v1 & 1) || v1 != v2);
    return result;
}

TRACEPOINT(trace_kvmclock_system_time, "now=%lu v1=%d v2=%d tsc=%lu time=%lu mul=%d shift=%d fl=%d off=%ld", u64, u32, u32, u64, u64, u32, int, int, s64);

u64 percpu_pvclock::time()
{
    irq_save_lock_type irqlock;
    SCOPE_LOCK(irqlock);

    u32 v1, v2;
    u64 time, tsc;

    do {
        v1 = _vcpu_info->version;
        barrier();

        processor::lfence();
        tsc = processor::rdtsc();
        time = transform(_vcpu_info->params, tsc);

        // if (v1 != _version) {
        //     if (_version > 0) {
        //         _time_offset = transform(_params, tsc) + _time_offset - time;
        //     }

        //     assert(abs((s64)_time_offset) < 10000);

        //     _params = _vcpu_info->params;
        // }

        _params = _vcpu_info->params;

        time += _time_offset;

        barrier();
        v2 = _vcpu_info->version;
    } while ((v1 & 1) || v1 != v2);

    if (_version != v1) {
        trace_kvmclock_system_time(tsc, _version, v1,
                _params.tsc_timestamp, _params.system_time,
                 _params.tsc_to_system_mul, _params.tsc_shift, _vcpu_info->flags, (s64) _time_offset);
    }

    _version = v1;

    assert(abs((s64)_time_offset) < 10000);

    assert(time >= _vcpu_info->last);
    _vcpu_info->last = time;
    return time;
}

u64 system_time(pvclock_vcpu_time_info* sys)
{
    return read_atomic(sys, [] (pvclock_vcpu_time_info* info) -> u64 {
        processor::lfence();
        return transform(info->params, processor::rdtsc());
    });
}

u64 percpu_pvclock::processor_to_nano(u64 time)
{
    return processor_to_nano2(_vcpu_info->params, time);
}

};
