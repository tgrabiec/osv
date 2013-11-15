#include <osv/trace.hh>
#include <osv/guards.hh>
#include "sched.hh"
#include "debug.hh"
#include "safe-ptr.hh"
#include <boost/functional/hash.hpp>
#include <osv/lockdep.hh>
#include <osv/hashing.hh>
#include <lockfree/hash_set.hh>

namespace lockdep {

int max_held = 0;
int n_erased = 0;

class lock_pair {
public:
    const lock_id_t lock1;
    const lock_id_t lock2;

    trace lock1_trace;
    trace lock2_trace;

    sched::thread* thread;

    lock_pair(lock_id_t lock1, lock_id_t lock2) : lock1(lock1), lock2(lock2) {}

    // two lock_pairs are equal if the set of locks they hold is the same regardless
    // of the lock order. So: equal({A,B}, {B,A}) && hash({A,B}) == hash({B,A})

    bool is_reverse_of(const lock_pair& other) const {
        return lock1 == other.lock2 && lock2 == other.lock1;
    }

    bool operator==(const lock_pair& pair2) {
        return (lock1 == pair2.lock1 && lock2 == pair2.lock2) || // (A, B) == (A, B)
               (lock1 == pair2.lock2 && lock2 == pair2.lock1);   // (A, B) == (B, A)
    }
};

struct lock_pair_hash {
    size_t operator()(const lock_pair& pair) const {
        // Needs to be symmetric
        return hash<size_t>(pair.lock1) ^ hash<size_t>(pair.lock2);
    }
};

static void print_trace(trace& bt) {
    for (auto ip : bt) {
        debug_ll("    %p\n", ip - 1);
    }
    debug_ll("    ...\n");
}

struct violation {
    lock_pair *previous;
    trace current_lock1_trace;
    trace current_lock2_trace;
    sched::thread* current_thread;

    void print() {
        debug_ll("lock A (attempted) : %p \n", previous->lock1);
        debug_ll("lock B (held)      : %p \n", previous->lock2);

        debug_ll("\nThread %p:\n", current_thread);
        debug_ll("  acquired A at:\n");
        print_trace(current_lock2_trace);
        debug_ll("\n  and now tries to acquire B at:\n");
        print_trace(current_lock1_trace);

        debug_ll("\nPreviously, thread %p:\n", previous->thread);
        debug_ll("  acquired B at:\n");
        print_trace(previous->lock1_trace);
        debug_ll("\n  and then tried to acquire A at:\n");
        print_trace(previous->lock2_trace);
    }

    bool operator==(const violation& other) {
        return previous->lock1_trace == other.previous->lock1_trace &&
               previous->lock2_trace == other.previous->lock2_trace &&
               current_lock1_trace == other.current_lock1_trace &&
               current_lock2_trace == other.current_lock2_trace;
    }
};

struct violation_hash {
    trace_hash bt_hash;

    size_t operator()(const violation& v) const {
        size_t h = 0;
        h += 31 * bt_hash(v.previous->lock1_trace);
        h += 31 * bt_hash(v.previous->lock2_trace);
        h += 31 * bt_hash(v.current_lock1_trace);
        h += 31 * bt_hash(v.current_lock2_trace);
        return h;
    }
};

static const int LOCK_DEP_TABLE_SIZE = 16000000;
static const int VIOLATION_TABLE_SIZE = 1000;
static const int LOCKDEP_MEMPOOL_SIZE = 160000000;

static lockfree::hash_set<lock_pair, lock_pair_hash>* dependencies;
static lockfree::hash_set<violation, violation_hash>* violations;
static std::atomic<lock_id_t> next_lock_id(0);
static bool initialized = false;

static void print_stats(lockfree::hash_table_stats stats) {
    debug_ll("n_slots:        %d\n", stats.n_slots);
    debug_ll("n_elements:     %d\n", stats.n_elements);
    debug_ll("max_collisions: %d\n", stats.max_collisions);
}

static void print_state_info();

class lockfree_pool {
private:
    int _size;
    std::atomic<int> n_allocated;
    char* _pool;
public:
    lockfree_pool(int size) : _size(size), n_allocated(0) {
        _pool = new char[size];
    }

    template<typename X, typename ...Args>
    X* alloc(Args&& ...args) {
        auto size = sizeof(X);
        auto slot = n_allocated.fetch_add(size);
        if (slot + size > _size) {
            debug_ll("lockdep: memory pool exhausted\n");
            print_state_info();
            abort();
        }

        return new (_pool + slot) X(std::forward<Args>(args)...);
    }

    int get_size() {
        return _size;
    }
};

static lockfree_pool* mempool;

static void print_state_info() {
    debug_ll("dependency hash table\n");
    auto dep_table_stats = dependencies->get_stats();
    print_stats(dep_table_stats);

    debug_ll("\nviolations hash table:\n");
    auto violation_table_stats = violations->get_stats();
    print_stats(violation_table_stats);

    auto n_classes = next_lock_id.load();

    debug_ll("\n");
    debug_ll("max lock chain length: %d\n", max_held);
    debug_ll("locks destroyed:       %d\n", n_erased);
    debug_ll("lock classes:          %d\n", next_lock_id.load());

    debug_ll("\nMemory pool\n");
    debug_ll("capacity:              %d\n", mempool->get_size());
    debug_ll("lock_pair footprint:   %d\n", sizeof(lock_pair) * dep_table_stats.n_elements);
    debug_ll("violation footprint:   %d\n", sizeof(violation) * violation_table_stats.n_elements);
    debug_ll("lock_tag footprint:    %d\n", sizeof(lock_tag) * n_classes);
}

void init_lockdep() {
    dependencies = new lockfree::hash_set<lock_pair, lock_pair_hash>(LOCK_DEP_TABLE_SIZE, 0.001);
    violations = new lockfree::hash_set<violation, violation_hash>(VIOLATION_TABLE_SIZE);
    mempool = new lockfree_pool(LOCKDEP_MEMPOOL_SIZE);
    initialized = true;
}

static context* get_context(sched::thread* current) {
    if (!initialized)
        return nullptr;

    if (current == nullptr)
        return nullptr;

    return current->lockdep_context;
}

static __thread bool recursion_guard = false;
#define LOCKDEP_NON_REENTRANT() NON_REENTRANT(recursion_guard)

static lock_id_t new_lock_id() {
    return next_lock_id.fetch_add(1);
}

static lock_tag& tag(lock_hook& hook) {
    auto& ptr_holder = hook.tag;
    auto old_ptr = ptr_holder.load(std::memory_order_acquire);
    if (old_ptr)
        return *old_ptr;

    auto new_ptr = mempool->alloc<lock_tag>(new_lock_id());
    if (!ptr_holder.compare_exchange_strong(old_ptr, new_ptr, std::memory_order_release)) {
        // we leak new_ptr, it shouldn't happen often though
        return *old_ptr;
    }

    return *new_ptr;
}

static void report_inversion(lock_tag& held_lock, lock_tag& lock, lock_pair& existing, trace& current_bt) {
    violation* v = new (mempool->alloc<violation>()) violation;

    v->previous = &existing;
    v->current_lock1_trace = current_bt;
    v->current_lock2_trace = held_lock.current_acquisition_trace;
    v->current_thread = sched::thread::current();

    violation* old_violation;
    if (!violations->add_if_absent(old_violation, v))
        abort();

    if (!old_violation) {
        // v->print();
        // abort();
    }
}

template<typename Lock, lock_hook Lock::*hook>
void lock_tracker<Lock, hook>::on_attempt(sched::thread* current, Lock* lock) {
    LOCKDEP_NON_REENTRANT() {
        auto ctx = get_context(current);
        if (!ctx)
            return;

        int n_held = 0;
        
        auto& l_tag = tag(lock->*hook);
        for (auto& held_lock : ctx->held_locks) {
            n_held++;

            if (held_lock.get_id() == l_tag.get_id())
                continue;

            {
                lock_pair pair{held_lock.get_id(), l_tag.get_id()};
                auto existing = dependencies->get(&pair);
                if (existing) {
                     if (existing->is_reverse_of(pair)) {
                        trace bt;
                        bt.fill_in();
                        report_inversion(held_lock, l_tag, *existing, bt);
                     }

                     continue;
                }
            }

            auto pair = mempool->alloc<lock_pair>(held_lock.get_id(), l_tag.get_id());
            pair->lock1_trace = held_lock.current_acquisition_trace;
            pair->lock2_trace.fill_in();
            pair->thread = current;

            lock_pair* existing;
            if (!dependencies->add_if_absent(existing, pair)) {
                debug_ll("Failed to add dependency, please increase table size.\n");
                print_state_info();
                abort();
            }

            if (existing && existing->is_reverse_of(*pair)) {
                report_inversion(held_lock, l_tag, *existing, pair->lock2_trace);
            }
        }

        max_held = std::max(n_held, max_held);
    }
}

template<typename Lock, lock_hook Lock::*hook>
void lock_tracker<Lock, hook>::on_acquire(sched::thread* current, Lock* lock) {
    LOCKDEP_NON_REENTRANT() {
        auto ctx = get_context(current);
        if (!ctx)
            return;

        auto& l_tag = tag(lock->*hook);
        ctx->held_locks.push_front(l_tag);
        l_tag.current_acquisition_trace.fill_in();
    }
}

static void release(context& ctx, lock_tag& l_tag) {
    l_tag.current_acquisition_trace.clear();
    auto& held = ctx.held_locks;
    held.erase(held.iterator_to(l_tag));
}

template<typename Lock, lock_hook Lock::*hook>
void lock_tracker<Lock, hook>::on_release(sched::thread* current, Lock* lock) {
    LOCKDEP_NON_REENTRANT() {
        auto ctx = get_context(current);
        if (!ctx)
            return;

        auto& l_tag = tag(lock->*hook);
        release(*ctx, l_tag);
    }
}

template<typename Lock, lock_hook Lock::*hook>
void lock_tracker<Lock, hook>::on_destroy(sched::thread* current, Lock* lock) {
    LOCKDEP_NON_REENTRANT() {
        auto ctx = get_context(current);
        if (!ctx)
            return;

        n_erased++;

        auto& lock_hook = lock->*hook;
        auto l_tag = lock_hook.tag.load(std::memory_order_acquire);
        if (!l_tag)
            return;

        if (l_tag->held_hook.is_linked()) {
            release(*ctx, *l_tag);
        }
    }
}

template<typename Lock, lock_hook Lock::*hook>
void lock_tracker<Lock, hook>::set_class(Lock* lock, lockdep_lock_class* lock_class) {
        auto& lock_hook = lock->*hook;

        lock_tag* old_ptr = nullptr;
        auto new_ptr = mempool->alloc<lock_tag>(lock_class->id);
        if (!lock_hook.tag.compare_exchange_strong(old_ptr, new_ptr, std::memory_order_release)) {
            debug_ll("Lock tag already assigned");
            abort();
        }
}

extern "C" void lockdep_new_class(lockdep_lock_class* lock_class, const char* name) {
    lock_class->id = new_lock_id();
    lock_class->name = name;
}

template class lock_tracker<lockfree::mutex, &lockfree::mutex::lockdep_hook>;

}
