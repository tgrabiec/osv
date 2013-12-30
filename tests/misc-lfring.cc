/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

//
// Instructions: run this test with 4 vcpus
//
#include <cstdlib>
#include <ctime>
#include <sched.hh>
#include <arch.hh>
#include <drivers/clock.hh>
#include <debug.hh>
#include <lockfree/ring.hh>
#include <lockfree/queue-mpsc.hh>

//
// Create 2 threads on different CPUs which perform concurrent push/pop
// Testing spsc ring
//
class test_spsc_ring {
public:
    test_spsc_ring() : _ring(4096) { }

    static const int max_random = 25;
    static const u64 elements_to_process = 300000000;

    bool run()
    {
        assert (sched::cpus.size() >= 2);

        sched::thread * thread1 = new sched::thread([&] { thread_push(0); },
            sched::thread::attr().pin(sched::cpus[0]));
        sched::thread * thread2 = new sched::thread([&] { thread_pop(1); },
            sched::thread::attr().pin(sched::cpus[1]));

        thread1->start();
        thread2->start();

        thread1->join();
        thread2->join();

        delete thread1;
        delete thread2;

        bool success = true;
        debug("Results:\n");
        for (int i=0; i < max_random; i++) {
            unsigned pushed = _stats[0][i];
            unsigned popped = _stats[1][i];

            debug("    value=%-08d pushed=%-08d popped=%-08d\n", i,
                pushed, popped);

            if (pushed != popped) {
                success = false;
            }
        }

        return success;
    }

private:

    ring_spsc<int> _ring;

    int _stats[2][max_random] = {};

    void thread_push(int cpu_id)
    {
        std::srand(std::time(0));
        for (u64 ctr=0; ctr < elements_to_process; ctr++)
        {
            int element = std::rand() % max_random;
            while (!_ring.push(element));
            _stats[cpu_id][element]++;
        }
    }

    void thread_pop(int cpu_id)
    {
        std::srand(std::time(0));
        for (u64 ctr=0; ctr < elements_to_process; ctr++)
        {
            int element = 0;
            while (!_ring.pop(element));
            _stats[cpu_id][element]++;
        }
    }
};

//
// Create 4 threads on different CPUs which perform concurrent push/pop
// Testing mpsc ring
//
class test_mpsc_ring {
public:

    static const int max_random = 25;
    static const u64 elements_to_process = 100000000;

    bool run()
    {
        assert (sched::cpus.size() >= 4);

        sched::thread * thread1 = new sched::thread([&] { thread_push(0); },
            sched::thread::attr().pin(sched::cpus[0]));
        sched::thread * thread2 = new sched::thread([&] { thread_push(1); },
            sched::thread::attr().pin(sched::cpus[1]));
        sched::thread * thread3 = new sched::thread([&] { thread_push(2); },
            sched::thread::attr().pin(sched::cpus[2]));
        sched::thread * thread4 = new sched::thread([&] { thread_pop(3); },
            sched::thread::attr().pin(sched::cpus[3]));

        thread1->start();
        thread2->start();
        thread3->start();
        thread4->start();

        thread1->join();
        thread2->join();
        thread3->join();
        thread4->join();

        delete thread1;
        delete thread2;
        delete thread3;
        delete thread4;

        bool success = true;
        debug("Results:\n");
        for (int i=0; i < max_random; i++) {
            unsigned pushed = _stats[0][i] + _stats[1][i] + _stats[2][i];
            unsigned popped = _stats[3][i];

            debug("    value=%-08d pushed=%-08d popped=%-08d\n", i,
                pushed, popped);

            if (pushed != popped) {
                success = false;
            }
        }

        return success;
    }

private:

    ring_mpsc<int, 4096> _ring;

    int _stats[4][max_random] = {};

    void thread_push(int cpu_id)
    {
        std::srand(std::time(0));
        for (u64 ctr=0; ctr < elements_to_process; ctr++)
        {
            int element = std::rand() % max_random + 6;
            unsigned in_idx = _ring.push(element);
            if (in_idx != 0)
                while (!_ring.push_to(element, in_idx));
            _stats[cpu_id][element-6]++;
        }
    }

    void thread_pop(int cpu_id)
    {
        std::srand(std::time(0));
        for (u64 ctr=0; ctr < elements_to_process*3; ctr++)
        {
            int element = 0;
            while (!_ring.pop(element));
            _stats[cpu_id][element-6]++;
        }
    }
};


class test_mpsc_queue {
public:

    static const int max_random = 25;
    static const u64 elements_to_process = 1000000;

    void init()
    {
        std::srand(std::time(0));
        for (unsigned i=0; i < elements_to_process; i++) {
            _items[0][i].value = std::rand() % max_random;
            _items[1][i].value = std::rand() % max_random;
            _items[2][i].value = std::rand() % max_random;
        }
    }

    bool run()
    {
        assert (sched::cpus.size() >= 4);

        sched::thread * thread1 = new sched::thread([&] { thread_push(0); },
            sched::thread::attr().pin(sched::cpus[0]));
        sched::thread * thread2 = new sched::thread([&] { thread_push(1); },
            sched::thread::attr().pin(sched::cpus[1]));
        sched::thread * thread3 = new sched::thread([&] { thread_push(2); },
            sched::thread::attr().pin(sched::cpus[2]));
        sched::thread * thread4 = new sched::thread([&] { thread_pop(3); },
            sched::thread::attr().pin(sched::cpus[3]));

        thread1->start();
        thread2->start();
        thread3->start();
        thread4->start();

        thread1->join();
        thread2->join();
        thread3->join();
        thread4->join();

        delete thread1;
        delete thread2;
        delete thread3;
        delete thread4;

        bool success = true;
        debug("Results:\n");
        for (int i=0; i < max_random; i++) {
            unsigned pushed = _stats[0][i] + _stats[1][i] + _stats[2][i];
            unsigned popped = _stats[3][i];

            debug("    value=%-08d pushed=%-08d popped=%-08d\n", i,
                pushed, popped);

            if (pushed != popped) {
                success = false;
            }
        }

        return success;
    }

private:

    lockfree::queue_mpsc<lockfree::linked_item<int>> _queue;

    // items for pusher1, pusher2, pusher3
    lockfree::linked_item<int> _items[3][elements_to_process];

    int _stats[4][max_random] = {};

    void thread_push(int cpu_id)
    {
        for (u64 ctr=0; ctr < elements_to_process; ctr++)
        {
            auto item = &_items[cpu_id][ctr];
            _queue.push(item);
            _stats[cpu_id][item->value]++;
        }
    }

    void thread_pop(int cpu_id)
    {
        for (u64 ctr=0; ctr < elements_to_process*3; ctr++)
        {
            lockfree::linked_item<int> *it = nullptr;
            while (!(it = _queue.pop()));
            _stats[cpu_id][it->value]++;
        }
    }
};

int main(int argc, char **argv)
{
    // Test
    debug("[~] Testing mpsc-queue:\n");
    test_mpsc_queue *t3 = new test_mpsc_queue();
    t3->init();
    s64 beg = nanotime();
    bool rc = t3->run();
    s64 end = nanotime();
    delete t3;
    if (rc) {
        double dT = (double)(end-beg)/1000000000.0;
        debug("[+] mpsc-queue test passed:\n");
        debug("[+] duration: %.6fs\n", dT);
        debug("[+] throughput: %.0f ops/s\n", (double)(test_mpsc_queue::elements_to_process*6)/dT);
    } else {
        debug("[-] mpsc-queue failed\n");
        return 1;
    }


    debug("[~] Testing spsc ringbuffer:\n");
    test_spsc_ring t1;
    beg = nanotime();
    rc = t1.run();
    end = nanotime();
    if (rc) {
        double dT = (double)(end-beg)/1000000000.0;
        debug("[+] spsc test passed:\n");
        debug("[+] duration: %.6fs\n", dT);
        debug("[+] throughput: %.0f ops/s\n", (double)(test_spsc_ring::elements_to_process*2)/dT);
    } else {
        debug("[-] spsc test failed\n");
        return 1;
    }

    debug("[~] Testing mpsc ringbuffer:\n");
    test_mpsc_ring t2;
    beg = nanotime();
    rc = t2.run();
    end = nanotime();
    if (rc) {
        double dT = (double)(end-beg)/1000000000.0;
        debug("[+] mpsc test passed:\n");
        debug("[+] duration: %.6fs\n", dT);
        debug("[+] throughput: %.0f ops/s\n", (double)(test_mpsc_ring::elements_to_process*6)/dT);
    } else {
        debug("[-] mpsc test failed\n");
        return 1;
    }

    debug("[+] finished.\n");
    return 0;
}
