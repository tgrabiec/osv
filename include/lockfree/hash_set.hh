#ifndef _LF_HSET_HH
#define _LF_HSET_HH

#include <atomic>
#include <assert.h>
#include <stdexcept>
#include <functional>

namespace lockfree {

struct hash_table_stats {
    int n_slots;
    int n_elements;
    int max_collisions;
};

/**
 *  Fixed-capacity lock-free malloc-free(*) no-throw monotonic(**) hash set.
 *
 *  It's useful to hold shared data which is frequently queried but rarely
 *  inserted.
 *
 *   (*) alocates only during construction.
 *
 *   (**) monotonic means that data is only added, never removed. This makes
 *        for simpler and more efficient implementation.
 */

template<typename T, typename Hash>
class hash_set {
public:

    hash_set(int n_slots, float collision_fraction = 0.01) 
        : _n_slots(n_slots),
          max_collisions(std::max(1, (int)(n_slots * collision_fraction))),
          _hash_fn(Hash())
    {
        assert(max_collisions < n_slots);

        slots = new std::atomic<T*>[n_slots];
        for (int i = 0; i < n_slots; i++) {
            slots[i].store(nullptr, std::memory_order_relaxed);
        }
    }

    hash_set(lockfree::hash_set<T, Hash>&& other)
        : _n_slots(other._n_slots),
          max_collisions(other.max_collisions),
          _hash_fn(other._hash_fn),
          slots(other.slots)
    {}

    /**
     * @returns false if insertion was needed but failed due to table full condition, true otherwise.
     */
    bool add_if_absent(T*& existing, T* element) {
        int collisions = 0;
        for (int slot = hash(element); collisions < max_collisions; slot = next_slot(slot), collisions++) {
            existing = slots[slot].load(std::memory_order_acquire);
            if (existing) { 
                if (*existing == *element) {
                    return true;
                }

                continue;
            }

            if (slots[slot].compare_exchange_strong(existing, element, std::memory_order_release)) {
                return true;
            }
        }

        return false;
    }

    T* get(T* element) {
        // We need to check at most max_collisions slots, then we're sure it's not there.
        int collisions = 0;
        for (int slot = hash(element); collisions < max_collisions; slot = next_slot(slot), collisions++) {
            T* existing = slots[slot].load(std::memory_order_acquire);
            if (!existing || *existing == *element)
                return existing;
        }

        return nullptr;
    }

    bool contains(T* element) {
        return get(element) != nullptr;
    }

    hash_table_stats get_stats() {
        int n_elements = 0;
        for (int slot = 0; slot < _n_slots; slot++) {
            auto element = slots[slot].load(std::memory_order_relaxed);
            if (element)
                n_elements++;
        }

        hash_table_stats stats;
        stats.n_slots = _n_slots;
        stats.n_elements = n_elements;
        stats.max_collisions = max_collisions;
        return stats;
    }

private:
    const int _n_slots;
    const int max_collisions;
    const Hash _hash_fn;
    std::atomic<T*>* slots;

    inline int next_slot(int slot) const {
        if (slot < _n_slots - 1)
            return slot + 1;
        return 0;
    }

    inline int hash(T const * element) const {
        auto slot = (int)(_hash_fn(*element) % _n_slots);
        return std::abs(slot);
    }
};

}

#endif
