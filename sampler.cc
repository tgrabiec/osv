#include <chrono>

#include "drivers/clock.hh"
#include "osv/sched.hh"
#include "osv/interrupt.hh"
#include "osv/debug.hh"
#include "osv/clock.hh"
#include "osv/trace.hh"
#include "osv/execinfo.hh"
#include "osv/percpu.hh"
#include "osv/sampler.hh"

namespace prof {

TRACEPOINT(trace_sampler_tick, "");

/*
 *   NOT_STARTED -> STARTING -> STARTED -> SHUTTING_DOWN
 *       ^                                     |
 *       `-------------------------------------'
 */
enum class sampling_state {
    NOT_STARTED,    // Sampling not active
    STARTING,       // Sampling initiated but not started yet on all CPUs
    STARTED,        // Sampling started on all CPUs
    SHUTTING_DOWN   // Shutdown initiatied
};

static std::atomic<unsigned int> nr_cpus_active {0};
static std::atomic<sampling_state> _state {sampling_state::NOT_STARTED};
static config _config;

class cpu_sampler : public sched::timer_base::client {
private:
    sched::timer_base _timer;

    void rearm() {
        _timer.set(osv::clock::uptime::now() + _config.period);
    }

public:
    cpu_sampler() : _timer(*this) {}

    void timer_fired() {
        rearm();
        trace_sampler_tick();
    }

    void start() {
        rearm();

        if (++nr_cpus_active == sched::cpus.size()) {
            _state.store(sampling_state::STARTED);
            debug("Sampler started on all CPUs\n");
        }
    }

    void stop() {
        _timer.cancel();

        if (--nr_cpus_active == 0) {
            _state.store(sampling_state::NOT_STARTED);
            debug("Sampler stopped on all CPUs\n");
        }
    }
};

static dynamic_percpu<cpu_sampler> _sampler;

static inter_processor_interrupt start_sampler_ipi{[] {
    _sampler->start();
}};

static inter_processor_interrupt stop_sampler_ipi{[] {
    _sampler->stop();
}};

template<typename Duration>
static long to_nanoseconds(Duration duration)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
}

bool start_sampler(config new_config)
{
    auto expected = sampling_state::NOT_STARTED;
    if (!_state.compare_exchange_strong(expected, sampling_state::STARTING)) {
        return false;
    }

    debug("Starting sampler, period = %d ns\n", to_nanoseconds(new_config.period));

    _config = new_config;
    std::atomic_thread_fence(std::memory_order_release);

    start_sampler_ipi.send_allbutself();
    _sampler->start();
    return true;
}

bool stop_sampler() {
    auto expected = sampling_state::STARTED;
    if (!_state.compare_exchange_strong(expected, sampling_state::SHUTTING_DOWN)) {
        return false;
    }

    debug("Stopping sampler\n");

    stop_sampler_ipi.send_allbutself();
    _sampler->stop();
    return true;
}

}
