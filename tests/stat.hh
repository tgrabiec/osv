#include <chrono>
#include <iostream>
#include <thread>
#include <atomic>
#include <future>

template<typename T>
static float to_seconds(T duration)
{
    return std::chrono::duration<float>(duration).count();
}

class stat_printer {
private:
    std::chrono::high_resolution_clock _clock;
    std::promise<bool> _done;
    std::atomic<long>& _counter;
    std::function<void(float)> _formatter;
    std::chrono::milliseconds stat_period;
    std::thread _thread;
public:
    stat_printer(std::atomic<long>& counter,
        std::function<void(float)> formatter,
        int period_millis = 500) :
         _clock(),
         _counter(counter),
         _formatter(formatter),
         stat_period(period_millis),
        _thread([&] {
            auto last_stat_dump = _clock.now();
            auto done_future = _done.get_future();
            while (true) {
                if (done_future.wait_until(last_stat_dump + stat_period) == std::future_status::ready) {
                    break;
                }

                auto _now = _clock.now();
                auto period = to_seconds(_now - last_stat_dump);
                last_stat_dump = _now;

                auto value = counter.exchange(0);

                _formatter((float)value / period);
            }
        }) {}

    void stop() {
        _done.set_value(true);
        _thread.join();
    }
};

template<typename Clock = std::chrono::high_resolution_clock>
class periodic {
public:
    using callback_t = std::function<void(typename Clock::duration)>;

    periodic(typename Clock::duration period, callback_t callback)
        : _period(period)
        , _callback(callback)
        , _thread([&]
    {
        auto last_stat_dump = Clock::now();
        auto done_future = _done.get_future();
        while (true) {
            if (done_future.wait_until(last_stat_dump + _period) == std::future_status::ready) {
                break;
            }
            auto _now = Clock::now();
            auto duration = _now - last_stat_dump;
            last_stat_dump = _now;
            _callback(duration);
        }
    }) {}

    ~periodic() {
        stop();
    }

    void stop() {
        _done.set_value(true);
        _thread.join();
    }

private:
    std::promise<bool> _done;
    typename Clock::duration _period;
    callback_t _callback;
    std::thread _thread;
};
