/*
 * BackgroundWorker
 *
 * A single thread that runs posted jobs one at a time. shutdown() drops
 * every job that has not started and waits until the job that is already
 * running returns. The running job is not interrupted; it has to notice
 * cancellation on its own and return. post() after shutdown() does nothing.
 */
#ifndef NPP_BACKGROUND_WORKER_H
#define NPP_BACKGROUND_WORKER_H

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

class BackgroundWorker {
public:
    BackgroundWorker();
    ~BackgroundWorker();

    BackgroundWorker(const BackgroundWorker &) = delete;
    BackgroundWorker &operator=(const BackgroundWorker &) = delete;

    void post(std::function<void()> fn);
    /* Replace all pending jobs with this one, leaving the running job alone.
     * Use only when the new job supersedes every pending result. */
    void post_latest(std::function<void()> fn);
    /* Runs on the worker thread after shutdown has been requested, the
     * running job has returned, and queued jobs have been dropped, and
     * before the thread exits. Set this before shutdown(). */
    void set_on_stop(std::function<void()> fn);
    void shutdown();
    bool stopping() const;

private:
    void loop();

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
    std::function<void()> on_stop_;
    bool stop_ = false;
    std::thread thread_;
};

#endif
