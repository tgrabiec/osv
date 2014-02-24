#ifndef _SAMPLER_HH
#define _SAMPLER_HH

#include "osv/clock.hh"

namespace prof {

struct config {
    osv::clock::uptime::duration period;
};

/**
 * Starts the profiler. It will hit sampler_tick tracepoint at given
 * frequency on all CPUs.
 *
 * Returns true if starting has been initiated, false if sampler is already starting or started.
 */
bool start_sampler(config);

/**
 * Stops the profiler.
 *
 * Returns true if stopping has been initiated, false if already initiated or stopped.
 */
bool stop_sampler();

}

#endif
