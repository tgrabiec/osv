/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/app.hh>
#include <string>
#include <osv/run.hh>
#include <osv/power.hh>
#include <osv/trace.hh>
#include <functional>
#include <thread>
#include <libgen.h>
#include <errno.h>
#include <algorithm>
#include <boost/range/algorithm/transform.hpp>

using namespace boost::range;

extern int optind;

// Java uses this global variable (supplied by Glibc) to figure out
// aproximatively where the initial thread's stack end.
void *__libc_stack_end;

// We will handle all the initialization ourselves.
// Still, if objects are linked with the -z now flag, they may possibly
// require this symbol to run. The symbol, though, should never be reached.
extern "C" void __libc_start_main()
{
    abort("Invalid call to __libc_start_main");
}

namespace osv {

static thread_local shared_app_t current_app;

shared_app_t application::get_current()
{
    return current_app;
}

TRACEPOINT(trace_app_adopt_current, "app=%p", application*);

void application::adopt_current()
{
    if (current_app) {
        current_app->abandon_current();
    }

    trace_app_adopt_current(this);
    current_app = shared_from_this();
}

TRACEPOINT(trace_app_abandon_current, "app=%p", application*);

void application::abandon_current()
{
    trace_app_abandon_current(this);
    current_app.reset();
}

shared_app_t application::run(const std::vector<std::string>& args)
{
    return run(args[0], args);
}

shared_app_t application::run(const std::string& command, const std::vector<std::string>& args)
{
    auto app = std::make_shared<application>(command, args);
    app->start();
    return app;
}

application::application(const std::string& command, const std::vector<std::string>& args)
    : _args(args)
    , _command(command)
    , _termination_requested(false)
{
    try {
        _lib = elf::get_program()->get_library(_command);
    } catch(const std::exception &e) {
        throw launch_error(e.what());
    }

    if (!_lib) {
        throw launch_error("Failed to load object: " + command);
    }

    _main = _lib->lookup<int (int, char**)>("main");
    if (!_main) {
        throw launch_error("Failed looking up main");
    }
}

void application::start()
{
    // FIXME: we cannot create the thread inside the constructor because
    // the thread would attempt to call shared_from_this() before object
    // is constructed which is illegal.
    auto err = pthread_create(&_thread, NULL, [](void *app) -> void* {
        ((application*)app)->main();
        return nullptr;
    }, this);
    if (err) {
        throw launch_error("Failed to create the main thread, err=" + std::to_string(err));
    }
}

TRACEPOINT(trace_app_destroy, "app=%p", application*);

application::~application()
{
    trace_app_destroy(this);
}

TRACEPOINT(trace_app_join, "app=%p", application*);
TRACEPOINT(trace_app_join_ret, "return_code=%d", int);

int application::join()
{
    trace_app_join(this);
    auto err = pthread_join(_thread, NULL);
    assert(!err);
    trace_app_join_ret(_return_code);
    return _return_code;
}

TRACEPOINT(trace_app_main, "app=%p, cmd=%s", application*, const char*);
TRACEPOINT(trace_app_main_ret, "return_code=%d", int);

void application::main()
{
    trace_app_main(this, _command.c_str());

    adopt_current();

    __libc_stack_end = __builtin_frame_address(0);

    sched::thread::current()->set_name(_command);

    run_main();

    if (_return_code) {
        debug("program %s returned %d\n", _command.c_str(), _return_code);
    }

    trace_app_main_ret(_return_code);
}

void application::run_main(std::string path, int argc, char** argv)
{
    char *c_path = (char *)(path.c_str());
    // path is guaranteed to keep existing this function
    program_invocation_name = c_path;
    program_invocation_short_name = basename(c_path);

    auto sz = argc; // for the trailing 0's.
    for (int i = 0; i < argc; ++i) {
        sz += strlen(argv[i]);
    }

    std::unique_ptr<char []> argv_buf(new char[sz]);
    char *ab = argv_buf.get();
    char *contig_argv[argc + 1];

    for (int i = 0; i < argc; ++i) {
        if (i) {
            _cmdline += " ";
        }
        _cmdline += argv[i];
        size_t asize = strlen(argv[i]);
        memcpy(ab, argv[i], asize);
        ab[asize] = '\0';
        contig_argv[i] = ab;
        ab += asize + 1;
    }
    contig_argv[argc] = nullptr;

    // make sure to have a fresh optind across calls
    // FIXME: fails if run() is executed in parallel
    int old_optind = optind;
    optind = 0;
    _return_code = _main(argc, contig_argv);
    optind = old_optind;
}

void application::run_main()
{
    // C main wants mutable arguments, so we have can't use strings directly
    std::vector<std::vector<char>> mut_args;
    transform(_args, back_inserter(mut_args),
            [](std::string s) { return std::vector<char>(s.data(), s.data() + s.size() + 1); });
    std::vector<char*> argv;
    transform(mut_args.begin(), mut_args.end(), back_inserter(argv),
            [](std::vector<char>& s) { return s.data(); });
    auto argc = argv.size();
    argv.push_back(nullptr);
    run_main(_command, argc, argv.data());
}

TRACEPOINT(trace_app_termination_callback_added, "app=%p", application*);
TRACEPOINT(trace_app_termination_callback_fired, "app=%p", application*);

void application::on_termination_request(std::function<void()> callback)
{
    auto app = current_app;
    std::unique_lock<mutex> lock(app->_termination_mutex);
    if (app->_termination_requested) {
        lock.unlock();
        callback();
        return;
    }

    app->_termination_signal.connect(callback);
}

TRACEPOINT(trace_app_request_termination, "app=%p, requested=%d", application*, bool);
TRACEPOINT(trace_app_request_termination_ret, "");

void application::request_termination()
{
    WITH_LOCK(_termination_mutex) {
        trace_app_request_termination(this, _termination_requested);
        if (_termination_requested) {
            trace_app_request_termination_ret();
            return;
        }
        _termination_requested = true;
    }

    if (current_app.get() == this) {
        _termination_signal();
    } else {
        std::thread terminator([&] {
            adopt_current();
            _termination_signal();
        });
        terminator.join();
    }

    trace_app_request_termination_ret();
}

int application::get_return_code()
{
    return _return_code;
}

std::string application::get_command()
{
    return _command;
}

std::string application::get_cmdline()
{
    return _cmdline;
}

namespace this_application {

void on_termination_request(std::function<void()> callback)
{
    application::on_termination_request(callback);
}

}

}
