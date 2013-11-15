#ifndef _LOCKDEP_H
#define _LOCKDEP_H

typedef int lockdep_lock_id_t;

#ifdef __cplusplus

namespace lockdep {
    typedef lockdep_lock_id_t lock_id_t;
}

#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    lockdep_lock_id_t id;
    const char *name;
} lockdep_lock_class;

void lockdep_new_class(lockdep_lock_class* lock_class, const char* name);

#ifdef __cplusplus
}
#endif

#endif
