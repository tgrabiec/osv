#ifndef _GUARDS_HH
#define _GUARDS_HH

struct non_reentrant_guard {
    bool& _guard;

    non_reentrant_guard(bool& guard) : _guard(guard) {
        assert(!_guard);
        _guard = true;
    }

    ~non_reentrant_guard() {
        assert(_guard);
        _guard = false;
    }

    operator bool() const { return false; }
};

#define NON_REENTRANT(guard_var) \
    if (non_reentrant_guard guard{guard_var}) \
        std::abort(); \
    else

#endif