/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/sched.hh>
#include <lockfree/ring.hh>
#include <osv/mutex.h>
#include <osv/migration-lock.hh>
#include <vector>

/**
 *
 * Lock-less multiple-producer single-consumer collection.
 * Uses per-CPU rings to handle multiple writers.
 * Does not respect insertion order when draining.
 *
 * TODO: does not support CPU hot-plugging.
 */
template<class T, unsigned MaxSizePerCpu>
class unordered_ring_mpsc
{
private:
    std::vector<ring_spsc<T,MaxSizePerCpu>> rings;
public:
    using ring_mpsc_t = unordered_ring_mpsc<T,MaxSizePerCpu>;

    class draining_iterator : public std::iterator<std::input_iterator_tag, T> {
    private:
        int _idx;
        ring_mpsc_t& _ring;
        T _element;

        void advance()
        {
            if (_idx >= _ring.rings.size()) {
                return;
            }

            if (_ring.rings[_idx].pop(_element)) {
                return;
            }

            do {} while (++_idx < _ring.rings.size() && !_ring.rings[_idx].pop(_element));
        }
    public:
        draining_iterator(ring_mpsc_t& ring)
            : _idx(0)
            , _ring(ring)
        {
            advance();
        }

        void move_to_end() {
            _idx = _ring.rings.size();
        }

        void operator++()
        {
            advance();
        }

        T& operator*()
        {
            return _element;
        }

        bool operator==(const draining_iterator& other) const {
            return _idx == other._idx;
        }

        bool operator!=(const draining_iterator& other) const {
            return _idx != other._idx;
        }
    };

    class draining_range {
    private:
        ring_mpsc_t& _ring;
    public:
        using iterator = draining_iterator;

        draining_range(ring_mpsc_t& ring)
            : _ring(ring)
        {
        }

        iterator begin()
        {
            return draining_iterator(_ring);
        }

        iterator end() {
            auto it = draining_iterator(_ring);
            it.move_to_end();
            return it;
        }
    };

    unordered_ring_mpsc()
        : rings(sched::cpus.size())
    {
    }

    template<typename Constructor>
    inline bool emplace(Constructor&& constructor) noexcept
    {
        WITH_LOCK(migration_lock) {
            auto current_id = sched::cpu::current()->id;
            assert(current_id < rings.size());
            return rings.at(current_id).emplace(std::move(constructor));
        }
    }

    draining_range drain()
    {
        return draining_range(*this);
    }
};
