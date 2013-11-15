#ifndef _LOCKDEP_HH
#define _LOCKDEP_HH

#include <osv/backtrace.hh>
#include <boost/intrusive/list.hpp>
#include <osv/lockdep.h>

namespace sched {
    class thread;
}

namespace lockdep {

namespace bi = boost::intrusive;

typedef int lock_id_t;

class lock_tag {
private:
    const lock_id_t _id;
public:
    bi::list_member_hook<> held_hook;
    trace current_acquisition_trace;

    lock_tag(lock_id_t id) : _id(id) {}
    lock_tag(lock_tag&&) = delete;

    lock_id_t get_id() {
        return _id;
    }
};

struct lock_hook {
    std::atomic<lock_tag*> tag;

    constexpr lock_hook() : tag(nullptr) {}
};

typedef bi::list<lock_tag,
             bi::member_hook<lock_tag,
                         bi::list_member_hook<>,
                         &lock_tag::held_hook>>
        lock_tag_list_t;

struct context {
    lock_tag_list_t held_locks;
};

template<typename Lock, lock_hook Lock::*hook>
class lock_tracker {
public:

    /**
     * Call before lock attempt is made.
     */
    void on_attempt(sched::thread* current, Lock* lock);
    
    /**
     * Call after lock is acquired. Do not call on recursive acquire.
     */
    void on_acquire(sched::thread* current, Lock* lock);
    
    /**
     * Call before lock is released.
     */
    void on_release(sched::thread* current, Lock* lock);
    
    /**
     * Call when lock is deallocated. This is just a hint which improves
     * performance, it's not mandatory for corectness.
     */
    void on_destroy(sched::thread* current, Lock* lock);

    /**
     * Assign lock to a lock class.
     */
    void set_class(Lock* lock, lockdep_lock_class* lock_class);
};

void init_lockdep();

}

#endif
