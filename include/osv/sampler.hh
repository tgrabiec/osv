/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef _OSV_SAMPLER_HH
#define _OSV_SAMPLER_HH

#include <osv/clock.hh>

namespace prof {

struct config {
    osv::clock::uptime::duration period;
};

/**
 * Starts the sampler.
 *
 * Should be called after stop_sampler() returns or before any call to stop_sampler().
 * Throws std::runtime_error() if called at incorrect time.
 */
void start_sampler(config);

/**
 * Stops the sampler.
 *
 * Should be called after start_sampler() returns.
 * Throws std::runtime_error() if called at incorrect time.
 */
void stop_sampler();

}

#endif
