/*
 * CancellableSpawn
 *
 * Starts a child process and waits for it. Another thread may cancel()
 * the child, or retire() the spawner so later run() calls do not start
 * anything. Editor shutdown uses retire() so a model download cannot
 * hold the process open for curl's full timeout.
 */
#ifndef NPP_CANCELLABLE_SPAWN_H
#define NPP_CANCELLABLE_SPAWN_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

class CancellableSpawn {
public:
    struct Result {
        bool started = false;
        bool cancelled = false;
        bool exited = false;
        int exit_code = -1;
        std::string stderr_text;
        std::string spawn_error;
    };

    using Ticket = uint64_t;
    Ticket ticket() const { return cancel_gen_.load(); }
    bool cancelled(Ticket ticket) const { return retired_.load() || cancel_gen_.load() != ticket; }
    Result run(const std::vector<std::string> &args) { return run(args, ticket()); }
    Result run(const std::vector<std::string> &args, Ticket ticket);
    void cancel();
    void retire();
    bool retired() const { return retired_.load(); }

    /* Called before the launch lock. Tests use it to cancel in that window. */
    void set_pre_lock_hook(std::function<void()> hook);

private:
    std::mutex mu_;
    std::atomic<uint64_t> cancel_gen_{0};
    std::atomic<bool> retired_{false};
    std::function<void()> pre_lock_hook_;
    int pid_ = 0;
};

#endif
