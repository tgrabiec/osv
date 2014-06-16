/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include "clock.hh"
#include "msr.hh"
#include <osv/types.h>
#include <osv/mmu.hh>
#include "string.h"
#include "cpuid.hh"
#include <osv/barrier.hh>
#include <osv/percpu.hh>
#include <osv/pvclock-abi.hh>
#include <osv/prio.hh>
#include <mutex>
#include <atomic>

class kvmclock : public clock {
public:
    kvmclock();
    virtual s64 time() __attribute__((no_instrument_function));
    virtual s64 uptime() override __attribute__((no_instrument_function));
    virtual s64 boot_time() override __attribute__((no_instrument_function));
    virtual u64 processor_to_nano(u64 ticks) override __attribute__((no_instrument_function));
    static bool probe();
private:
    u64 wall_clock_boot();
    u64 system_time();
    void setup_cpu();
private:
    static std::atomic<bool> _smp_init;
    static std::once_flag _boot_systemtime_init_flag;
    static s64 _boot_systemtime;
    static bool _new_kvmclock_msrs;
    pvclock_wall_clock* _wall;
    static percpu<pvclock_vcpu_time_info> _sys;
    sched::cpu::notifier cpu_notifier;
    pvclock _pvclock;
};

std::atomic<bool> kvmclock::_smp_init { false };
std::once_flag kvmclock::_boot_systemtime_init_flag;
s64 kvmclock::_boot_systemtime = 0;
bool kvmclock::_new_kvmclock_msrs = true;
PERCPU(pvclock_vcpu_time_info, kvmclock::_sys);

static u8 get_pvclock_flags()
{
    u8 flags = 0;
    if (processor::features().kvm_clocksource_stable) {
        flags |= pvclock::TSC_STABLE_BIT;
    }
    return flags;
}

kvmclock::kvmclock()
    : cpu_notifier([&] { setup_cpu(); })
    , _pvclock(get_pvclock_flags())
{
    auto wall_time_msr = (_new_kvmclock_msrs) ?
                         msr::KVM_WALL_CLOCK_NEW : msr::KVM_WALL_CLOCK;
    _wall = new pvclock_wall_clock;
    memset(_wall, 0, sizeof(*_wall));
    processor::wrmsr(wall_time_msr, mmu::virt_to_phys(_wall));
}

void kvmclock::setup_cpu()
{
    auto system_time_msr = (_new_kvmclock_msrs) ?
                           msr::KVM_SYSTEM_TIME_NEW : msr::KVM_SYSTEM_TIME;
    memset(&*_sys, 0, sizeof(*_sys));
    processor::wrmsr(system_time_msr, mmu::virt_to_phys(&*_sys) | 1);

    std::call_once(_boot_systemtime_init_flag, [&] {
        _boot_systemtime = system_time();
        _smp_init.store(true, std::memory_order_release);
    });
}

bool kvmclock::probe()
{
    if (processor::features().kvm_clocksource2) {
        return true;
    }
    if (processor::features().kvm_clocksource) {
        _new_kvmclock_msrs = false;
        return true;
    }
    return false;
}

s64 kvmclock::time()
{
    auto r = wall_clock_boot();
    // FIXME: during early boot, while _smp_init is still false, we don't
    // add system_time() so we return the host's boot time instead of the
    // current time. When _smp_init becomes true, the clock jumps forward
    // to the correct current time.
    // This happens due to problems in init order dependencies (the clock
    // depends on the scheduler, for percpu initialization, and vice-versa,
    // for idle thread initialization).
    if (_smp_init.load(std::memory_order_acquire)) {
        r += system_time();
    }
    return r;
}

s64 kvmclock::uptime()
{
    if (_smp_init.load(std::memory_order_acquire)) {
        return system_time() - _boot_systemtime;
    } else {
        return 0;
    }
}

s64 kvmclock::boot_time()
{
    // The following is time()-uptime():
    auto r = wall_clock_boot();
    if (_smp_init.load(std::memory_order_acquire)) {
        r += _boot_systemtime;
    }
    return r;
}

u64 kvmclock::wall_clock_boot()
{
    return _pvclock.wall_clock_boot(_wall);
}

u64 kvmclock::system_time()
{
    sched::preempt_disable();
    auto sys = &*_sys;  // avoid recaclulating address each access
    auto r = _pvclock.system_time(sys);
    sched::preempt_enable();
    return r;
}

u64 kvmclock::processor_to_nano(u64 ticks)
{
    return pvclock::processor_to_nano(&*_sys, ticks);
}

static __attribute__((constructor(init_prio::clock))) void setup_kvmclock()
{
    if (kvmclock::probe()) {
        clock::register_clock(new kvmclock);
    }
}
