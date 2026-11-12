#include "CancellableSpawn.h"

#include <glib.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <thread>
#include <unistd.h>
#include <sys/wait.h>

namespace {

void drain_fd(int fd, std::string &out)
{
    char buf[1024];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof buf);
        if (n > 0) {
            out.append(buf, static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
}

} /* namespace */

CancellableSpawn::Result CancellableSpawn::run(const std::vector<std::string> &args, Ticket ticket)
{
    Result result;
    if (cancelled(ticket)) {
        result.cancelled = true;
        return result;
    }
    if (args.empty()) {
        result.spawn_error = "empty command";
        return result;
    }

    std::vector<std::string> storage = args;
    std::vector<char *> argv;
    argv.reserve(storage.size() + 1);
    for (std::string &arg : storage) argv.push_back(arg.data());
    argv.push_back(nullptr);

    std::function<void()> hook;
    {
        std::lock_guard<std::mutex> lock(mu_);
        hook = pre_lock_hook_;
    }
    if (hook) hook();

    {
        std::lock_guard<std::mutex> lock(mu_);
        if (retired_.load() || cancel_gen_.load() != ticket) {
            result.cancelled = true;
            return result;
        }
        pid_ = 0;
    }

    gint stderr_fd = -1;
    GPid pid = 0;
    GError *gerr = nullptr;
    /* Reap the child here. Without this flag GLib's SIGCHLD handler can
     * collect it first, and cancel() would not be able to wait for it. */
    gboolean ok = g_spawn_async_with_pipes(
        nullptr, argv.data(), nullptr,
        static_cast<GSpawnFlags>(G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                                 G_SPAWN_DO_NOT_REAP_CHILD),
        nullptr, nullptr, &pid, nullptr, nullptr, &stderr_fd, &gerr);
    if (!ok) {
        result.spawn_error = gerr ? gerr->message : "could not start process";
        if (gerr) g_error_free(gerr);
        return result;
    }
    result.started = true;

    int flags = fcntl(stderr_fd, F_GETFL, 0);
    if (flags >= 0) fcntl(stderr_fd, F_SETFL, flags | O_NONBLOCK);

    {
        std::lock_guard<std::mutex> lock(mu_);
        pid_ = static_cast<int>(pid);
        if (cancel_gen_.load() != ticket || retired_.load()) kill(pid_, SIGKILL);
    }

    int status = 0;
    for (;;) {
        drain_fd(stderr_fd, result.stderr_text);
        bool stop = cancel_gen_.load() != ticket || retired_.load();
        if (stop) {
            std::lock_guard<std::mutex> lock(mu_);
            if (pid_ > 0) kill(pid_, SIGKILL);
        }
        pid_t got = waitpid(pid, &status, WNOHANG);
        if (got == pid) break;
        if (got < 0 && errno != EINTR) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    drain_fd(stderr_fd, result.stderr_text);
    ::close(stderr_fd);
    g_spawn_close_pid(pid);

    {
        std::lock_guard<std::mutex> lock(mu_);
        pid_ = 0;
    }
    result.cancelled = cancel_gen_.load() != ticket || retired_.load();
    if (WIFEXITED(status)) {
        result.exited = true;
        result.exit_code = WEXITSTATUS(status);
    }
    return result;
}

void CancellableSpawn::cancel()
{
    std::lock_guard<std::mutex> lock(mu_);
    cancel_gen_.fetch_add(1);
    if (pid_ > 0) kill(pid_, SIGKILL);
}

void CancellableSpawn::set_pre_lock_hook(std::function<void()> hook)
{
    std::lock_guard<std::mutex> lock(mu_);
    pre_lock_hook_ = std::move(hook);
}

void CancellableSpawn::retire()
{
    retired_.store(true);
    cancel();
}
