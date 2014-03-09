/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <thread>
#include <iostream>
#include <unistd.h>
#include <osv/sched.hh>
#include <osv/clock.hh>
#include <bsd/porting/callout.h>
#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/variance.hpp>
#include <boost/accumulators/statistics/stats.hpp>
#include <boost/accumulators/statistics/mean.hpp>
#include "stat.hh"

using _clock = osv::clock::uptime;
using namespace osv::clock::literals;
using namespace boost::accumulators;

static bool _empty;

template<typename Clock>
class sync_section {
private:
    const int _n_threads;
    std::atomic<int> _arrived;
    typename Clock::duration _duration;
    typename Clock::time_point _end;

public:
    sync_section(int n_threads, typename Clock::duration duration)
        : _n_threads(n_threads)
        , _duration(duration)
    {
        _arrived.store(0);
    }

    bool is_end()
    {
        assert(_arrived.load(std::memory_order_relaxed) == _n_threads);
        return Clock::now() >= _end;
    }

    void arrive()
    {
        auto n_arrived = _arrived.fetch_add(1);
        assert(n_arrived < _n_threads);

        if (n_arrived == _n_threads - 1) {
            _end = Clock::now() + _duration;
        }

        while (_arrived.load(std::memory_order_relaxed) < _n_threads) {
            sched_yield();
        }
    }
};

static void callout_callback(void*)
{
}

int main(int argc, char const *argv[])
{
    int n_threads = 4;

    if (argc > 1) {
        n_threads = atoi(argv[1]);
    }

    if (argc > 2) {
        _empty = atoi(argv[2]);
    }

    std::cout << "nthreads = " << n_threads << std::endl;
    std::cout << "empty = " << _empty << std::endl;

    sync_section<_clock> _sync_section{n_threads, 10_s};
    std::atomic<long> total{0};

    std::atomic<long> counters[n_threads];
    std::thread* threads[n_threads];
    for (int i = 0; i < n_threads; i++) {
        auto& count = counters[i];
        count.store(0);

        threads[i] = new std::thread([&] {
            callout c;
            callout_init(&c, true);

            _sync_section.arrive();

            if (_empty) {
                while (!_sync_section.is_end()) {
                    count.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                while (!_sync_section.is_end()) {
                    callout_reset(&c, 1000, callout_callback, nullptr);
                    count.fetch_add(1, std::memory_order_relaxed);
                }
            }

            callout_stop(&c);
        });
    }

    periodic<_clock> stat_printer(500_ms, [&] (_clock::duration period) {
        long total = 0;
        for (int i = 0; i < n_threads; i++) {
            total += counters[i].exchange(0);
        }
        printf("%ld\n", total);
    });

    for (int i = 0; i < n_threads; i++) {
        threads[i]->join();
    }

    return 0;
}