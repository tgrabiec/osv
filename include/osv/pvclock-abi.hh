/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef _OSV_PVCLOCK_ABI_H_
#define _OSV_PVCLOCK_ABI_H_
#include <osv/types.h>

struct pvclock_wall_clock {
        u32   version;
        u32   sec;
        u32   nsec;
} __attribute__((__packed__));

struct pvclock_transformation_params {
        u64   tsc_timestamp;
        u64   system_time;
        u32   tsc_to_system_mul;
        s8    tsc_shift;
} __attribute__((__packed__));

struct pvclock_vcpu_time_info {
         u32   version;
         u32   pad0;
         pvclock_transformation_params params;
         u8    flags;
         u8    pad[2];
} __attribute__((__packed__)); /* 32 bytes */

namespace pvclock {

class percpu_pvclock {
private:
    u32 _version;
    u64 _time_offset;
    pvclock_transformation_params _params;
    pvclock_vcpu_time_info* _vcpu_info;
public:
    percpu_pvclock(pvclock_vcpu_time_info* vcpu_info)
        : _version(0)
        , _time_offset(0)
        , _vcpu_info(vcpu_info)
    {
    }

    u64 time();
    u64 processor_to_nano(u64 time);
};

u64 wall_clock_boot(pvclock_wall_clock *_wall);
u64 system_time(pvclock_vcpu_time_info *sys);
};
#endif
