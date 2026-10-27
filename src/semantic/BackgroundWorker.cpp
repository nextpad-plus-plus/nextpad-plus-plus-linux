#include "BackgroundWorker.h"

BackgroundWorker::BackgroundWorker() : thread_([this] { loop(); }) {}

BackgroundWorker::~BackgroundWorker()
{
    shutdown();
}

void BackgroundWorker::post(std::function<void()> fn)
{
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (stop_) return;
        queue_.push_back(std::move(fn));
    }
    cv_.notify_one();
}

void BackgroundWorker::post_latest(std::function<void()> fn)
{
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (stop_) return;
        queue_.clear();
        queue_.push_back(std::move(fn));
    }
    cv_.notify_one();
}

void BackgroundWorker::set_on_stop(std::function<void()> fn)
{
    std::lock_guard<std::mutex> lock(mu_);
    on_stop_ = std::move(fn);
}

void BackgroundWorker::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
        queue_.clear();
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

bool BackgroundWorker::stopping() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return stop_;
}

void BackgroundWorker::loop()
{
    for (;;) {
        std::function<void()> fn;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
            /* stop_ wins over anything still queued. Those jobs are dropped
             * so process exit does not run work that touches freed state. */
            if (stop_) {
                queue_.clear();
                fn = std::move(on_stop_);
                lock.unlock();
                if (fn) fn();
                return;
            }
            fn = std::move(queue_.front());
            queue_.pop_front();
        }
        fn();
    }
}
