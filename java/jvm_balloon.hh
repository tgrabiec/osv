/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef JVM_BALLOON_HH_
#define JVM_BALLOON_HH_

#include <jni.h>
#include <osv/mempool.hh>
#include "exceptions.hh"
#include <osv/mmu.hh>

class jvm_balloon_shrinker : public memory::shrinker {
public:
    explicit jvm_balloon_shrinker(JavaVM *vm);
    virtual size_t request_memory(size_t s);
    virtual size_t release_memory(size_t s);
private:
    JavaVM *_vm;
    int _attach(JNIEnv **env);
    void _detach(int status);
    // FIXME: It can grow, but we will ignore it for now.
    size_t _total_heap;
};

class balloon;
void jvm_balloon_fault(balloon *b, exception_frame *ef, mmu::jvm_balloon_vma *vma);
#endif
