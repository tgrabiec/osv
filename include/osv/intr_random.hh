/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef OSV_INTR_RANDOM_HH
#define OSV_INTR_RANDOM_HH

#include <sys/sys/random.h>

struct intr_entropy
{
    void* pc;
    int vector;
};

static inline void harvest_intr_randomness(exception_frame* frame)
{
    intr_entropy entropy { frame->get_pc(), frame->error_code };
    random_harvest(&entropy, sizeof(entropy), 1, RANDOM_INTERRUPT);
}

#endif
