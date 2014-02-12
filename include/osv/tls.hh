/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef TLS_HH_
#define TLS_HH_

struct thread_control_block {
    thread_control_block* self;
    void* tls_base;
};

#endif /* TLS_HH_ */
