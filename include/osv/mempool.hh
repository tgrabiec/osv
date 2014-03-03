/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef MEMPOOL_HH
#define MEMPOOL_HH

#include <cstdint>
#include <functional>
#include <list>
#include <boost/intrusive/set.hpp>
#include <boost/intrusive/list.hpp>
#include <osv/mutex.h>
#include <arch.hh>
#include <osv/pagealloc.hh>
#include <osv/percpu.hh>
#include <osv/condvar.h>
#include <osv/semaphore.hh>

namespace memory {

const size_t page_size = 4096;

extern size_t phys_mem_size;

void* alloc_phys_contiguous_aligned(size_t sz, size_t align);
void free_phys_contiguous_aligned(void* p);

void setup_free_memory(void* start, size_t bytes);

void debug_memory_pool(size_t *total, size_t *contig);

namespace bi = boost::intrusive;

// pre-mempool object smaller than a page
static constexpr size_t non_mempool_obj_offset = 8;

class pool {
public:
    explicit pool(unsigned size);
    ~pool();
    void* alloc();
    void free(void* object);
    unsigned get_size();
    static pool* from_object(void* object);
private:
    struct page_header;
    struct free_object;
private:
    bool have_full_pages();
    void add_page();
    static page_header* to_header(free_object* object);

    // should get called with the preemption lock taken
    void free_same_cpu(free_object* obj, unsigned cpu_id);
    void free_different_cpu(free_object* obj, unsigned obj_cpu);
private:
    unsigned _size;

    struct page_header {
        pool* owner;
        unsigned cpu_id;
        unsigned nalloc;
        bi::list_member_hook<> free_link;
        free_object* local_free;  // free objects in this page
    };

    static_assert(non_mempool_obj_offset < sizeof(page_header), "non_mempool_obj_offset too large");

    typedef bi::list<page_header,
                     bi::member_hook<page_header,
                                     bi::list_member_hook<>,
                                     &page_header::free_link>,
                     bi::constant_time_size<false>
                    > free_list_base_type;
    class free_list_type : public free_list_base_type {
    public:
        ~free_list_type() { assert(empty()); }
    };
    // maintain a list of free pages percpu
    dynamic_percpu<free_list_type> _free;
public:
    static const size_t max_object_size;
    static const size_t min_object_size;
};

struct pool::free_object {
    free_object* next;
};

struct page_range {
    explicit page_range(size_t size);
    size_t size;
    boost::intrusive::set_member_hook<> member_hook;
};

void free_initial_memory_range(void* addr, size_t size);
void enable_debug_allocator();

extern bool tracker_enabled;

enum class pressure { RELAXED, NORMAL, PRESSURE, EMERGENCY };

class shrinker {
public:
    explicit shrinker(std::string name);
    virtual ~shrinker() {}
    virtual size_t request_memory(size_t n) = 0;
    virtual size_t release_memory(size_t n) = 0;
    std::string name() { return _name; };

    bool should_shrink(ssize_t target) { return _enabled && (target > 0); }

    void deactivate_shrinker();
    void activate_shrinker();
private:
    std::string _name;
    int _enabled = 1;
};

class reclaimer_waiters: public semaphore {
public:
    explicit reclaimer_waiters() : semaphore(0) { }
    virtual  ~reclaimer_waiters() {}

    virtual void post(unsigned units = 1);
    bool has_waiters() { WITH_LOCK(_mtx) { return !_waiters.empty(); } }
};

class reclaimer {
public:
    reclaimer ();
    void wake();
    void wait_for_memory(size_t mem);
    void wait_for_minimum_memory();

    friend void start_reclaimer();
    friend class shrinker;
private:
    void _do_reclaim();
    // We could just check if the semaphore's wait_list is empty. But since we
    // don't control the internals of the primitives we use for the
    // implementation of semaphore, we are concerned that unlocked access may
    // mean mayhem. Locked access, OTOH, will possibly deadlock us if someone allocates
    // memory locked, and then we try to verify if we have waiters and need to hold the
    // same lock
    //std::atomic<int> _waiters_count = { 0 };
    //bool _has_waiters() { return _waiters_count.load() != 0; }

    reclaimer_waiters _oom_blocked; // Callers are blocked due to lack of memory
    condvar _blocked;     // The reclaimer itself is blocked waiting for pressure condition
    sched::thread *_thread;

    std::vector<shrinker *> _shrinkers;
    mutex _shrinkers_mutex;
    unsigned int _active_shrinkers = 0;
    bool _can_shrink();

    pressure pressure_level();
    ssize_t bytes_until_normal(pressure curr);
    ssize_t bytes_until_normal() { return bytes_until_normal(pressure_level()); }
};

namespace stats {
    size_t free();
    size_t total();
    size_t jvm_heap();
    void on_jvm_heap_alloc(size_t mem);
    void on_jvm_heap_free(size_t mem);
}
}

#endif
