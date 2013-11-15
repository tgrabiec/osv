#ifndef _BACKTRACE_HH
#define _BACKTRACE_HH

#include <osv/hashing.hh>
#include <osv/execinfo.hh>

/**
 * Stack trace holder. malloc-free.
 */
class trace {
private:
    const static int max_frames = 20;
    void* _trace[max_frames];
public:
    typedef void** iterator;
    
    constexpr trace() : _trace{nullptr,} {}

    void clear() {
        _trace[0] = nullptr;
    }

    bool operator==(const trace& other) const {
        for (int i = 0; i < max_frames; i++) {
            if (_trace[i] != other._trace[i])
                return false;

            if (!_trace[i])
                break;
        }

        return true;
    }

    iterator begin() const {
        return (iterator) _trace;
    }

    iterator end() const {
        int i = 0;
        while (i < max_frames && _trace[i])
            i++;
        return (iterator) _trace + i;
    }

    void fill_in() {
        backtrace_safe(_trace, max_frames);
    }
};

struct trace_hash {
    size_t operator()(const trace& bt) const {
        size_t h = 0;

        for (void *ip : bt)
            h += 31 * hash<void*>(ip);

        return h;
    }
};

#endif