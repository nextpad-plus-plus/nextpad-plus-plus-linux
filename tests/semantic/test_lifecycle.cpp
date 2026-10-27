/*
 * Shutdown and heatmap decisions that do not need a GTK display.
 *
 * The quit case runs a download stand-in (sleep 180) and a slow
 * embedding provider on the background worker, then shuts that worker
 * down the way the editor does on quit. The process must return without
 * waiting out the download. The colour case checks that a publish queued
 * for an older query does not put the old colours back, and that an
 * empty query or an empty hit list clears.
 */
#include "BackgroundWorker.h"
#include "CancellableSpawn.h"
#include "HeatmapApply.h"
#include "SemanticInterfaces.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <future>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_fails = 0;

void expect(bool ok, const std::string &msg)
{
    if (ok) return;
    std::cerr << "FAIL " << msg << "\n";
    ++g_fails;
}

class SlowEmbed final : public EmbeddingProvider {
public:
    explicit SlowEmbed(BackgroundWorker &worker) : worker_(worker) {}

    int dimension() const override { return 4; }
    int max_tokens() const override { return 8; }
    bool available() const override { return true; }
    std::string backend_name() const override { return "fake"; }
    size_t token_count(const std::string &) const override { return 1; }

    bool embed_batch(const std::vector<std::string> &,
                     std::vector<float> &,
                     std::string &error,
                     const SemanticBatchProgress &) override
    {
        called.fetch_add(1);
        /* Poll the worker instead of blocking inside one long call, which
         * is the same point a real batch returns between groups. */
        for (int i = 0; i < 9000; ++i) {
            if (worker_.stopping()) {
                error = "cancelled";
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        error = "fake embed was not cancelled";
        return false;
    }

    std::atomic<int> called{0};

private:
    BackgroundWorker &worker_;
};

using clock = std::chrono::steady_clock;

bool wait_until(const std::atomic<int> &value, int want, std::chrono::milliseconds limit)
{
    auto start = clock::now();
    while (value.load() < want) {
        if (clock::now() - start > limit) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

void test_quit_during_download()
{
    BackgroundWorker worker;
    CancellableSpawn spawn;
    SlowEmbed embed(worker);
    std::atomic<int> phase{0};
    std::atomic<bool> queued_ran{false};
    std::atomic<bool> download_cancelled{false};

    worker.post([&] {
        phase.store(1);
        CancellableSpawn::Result result = spawn.run({"sleep", "180"});
        download_cancelled.store(result.cancelled);
        phase.store(2);
        if (result.cancelled || spawn.retired()) return;
        std::string error;
        std::vector<float> out;
        embed.embed_batch({"should-not-run"}, out, error, {});
    });
    worker.post([&] { queued_ran.store(true); });

    expect(wait_until(phase, 1, std::chrono::seconds(2)), "download job did not start");
    /* Give the child time to exist so cancel has a process to kill. */
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto start = clock::now();
    spawn.retire();
    worker.shutdown();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - start);

    expect(elapsed < std::chrono::seconds(3),
           "quit waited " + std::to_string(elapsed.count()) + " ms");
    expect(phase.load() == 2, "in-flight download job did not return");
    expect(download_cancelled.load(), "in-flight download was not cancelled");
    expect(embed.called.load() == 0, "embed ran after the download was cancelled");
    expect(!queued_ran.load(), "a queued job ran after shutdown");

    std::atomic<bool> late{false};
    worker.post([&] { late.store(true); });
    expect(!late.load(), "post after shutdown ran a job");
}

void test_quit_during_embed()
{
    BackgroundWorker worker;
    CancellableSpawn spawn;
    SlowEmbed embed(worker);
    std::atomic<int> phase{0};

    worker.post([&] {
        CancellableSpawn::Result result = spawn.run({"true"});
        if (result.cancelled || !result.exited || result.exit_code != 0) {
            phase.store(-1);
            return;
        }
        std::string error;
        std::vector<float> out;
        embed.embed_batch({"slow"}, out, error, {});
        phase.store(error == "cancelled" ? 1 : -1);
    });

    expect(wait_until(embed.called, 1, std::chrono::seconds(2)), "slow embed did not start");
    auto start = clock::now();
    spawn.retire();
    worker.shutdown();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - start);

    expect(elapsed < std::chrono::seconds(3),
           "quit during embed waited " + std::to_string(elapsed.count()) + " ms");
    expect(phase.load() == 1, "slow embed did not observe cancellation");
}

struct HeatmapScreen {
    bool colours = false;
    int clears = 0;
    int paints = 0;

    void clear()
    {
        colours = false;
        ++clears;
    }
    void paint()
    {
        colours = true;
        ++paints;
    }
};

/* Same branch the search bar takes for a publish that reaches the UI thread. */
void deliver(HeatmapScreen &screen, bool dead,
             uint64_t result_build, uint64_t build_now,
             uint64_t result_query, uint64_t query_now,
             bool query_empty, bool hits_empty)
{
    switch (heatmap_apply(dead, result_build, build_now, result_query, query_now,
                          query_empty, hits_empty)) {
    case HeatmapApply::Skip:
        break;
    case HeatmapApply::Clear:
        screen.clear();
        break;
    case HeatmapApply::Paint:
        screen.paint();
        break;
    }
}

void test_on_stop_runs_on_worker()
{
    BackgroundWorker worker;
    std::atomic<int> started{0};
    std::atomic<int> job_finished{0};
    std::thread::id job_thread;
    std::thread::id stop_thread;
    std::atomic<bool> stop_after_job{false};

    worker.post([&] {
        job_thread = std::this_thread::get_id();
        started.store(1);
        while (!worker.stopping())
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        job_finished.store(1);
    });
    expect(wait_until(started, 1, std::chrono::seconds(2)), "worker job did not start");
    worker.set_on_stop([&] {
        stop_thread = std::this_thread::get_id();
        stop_after_job.store(job_finished.load() == 1);
    });
    worker.shutdown();
    expect(stop_after_job.load(), "stop callback ran before the job returned");
    expect(stop_thread == job_thread, "stop callback was not on the worker thread");
    expect(stop_thread != std::this_thread::get_id(), "stop callback ran on the caller");
}

void test_cancel_before_lock()
{
    CancellableSpawn spawn;
    spawn.set_pre_lock_hook([&spawn] { spawn.cancel(); });
    auto start = clock::now();
    CancellableSpawn::Result result = spawn.run({"sleep", "30"});
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - start);
    expect(result.cancelled, "cancel before the lock was ignored");
    expect(!result.started, "a process was started after cancel");
    expect(elapsed < std::chrono::seconds(1),
           "cancel before the lock waited " + std::to_string(elapsed.count()) + " ms");

    spawn.set_pre_lock_hook({});
    CancellableSpawn::Result again = spawn.run({"true"});
    expect(!again.cancelled && again.exited && again.exit_code == 0,
           "an earlier cancel stuck to the next run");
}

void test_stale_query_colours()
{
    HeatmapScreen screen;
    screen.colours = true;

    /* The user cleared the query. The bar has already removed colours,
     * and the query generation moved from 4 to 5. A publish scored for
     * generation 4 is still queued and must not paint those hits. */
    screen.clear();
    deliver(screen, false, 3, 3, 4, 5, false, false);
    expect(!screen.colours, "queued publish repainted after the query was cleared");

    screen.colours = true;
    screen.clear();
    deliver(screen, false, 3, 3, 4, 6, false, false);
    expect(!screen.colours, "queued publish repainted after the query was replaced");

    screen.colours = true;
    deliver(screen, false, 3, 3, 7, 7, true, false);
    expect(!screen.colours, "empty query left colours on screen");

    screen.colours = true;
    deliver(screen, false, 3, 3, 7, 7, false, true);
    expect(!screen.colours, "empty hits left colours on screen");

    screen.colours = false;
    deliver(screen, false, 3, 3, 7, 7, false, false);
    expect(screen.colours, "current hits were not painted");

    screen.colours = true;
    int clears_before = screen.clears;
    int paints_before = screen.paints;
    deliver(screen, false, 2, 3, 7, 7, true, true);
    expect(screen.colours, "older build cleared the current heatmap");
    expect(screen.clears == clears_before && screen.paints == paints_before,
           "older build changed the current heatmap");

    deliver(screen, true, 3, 3, 7, 7, false, false);
    expect(screen.colours && screen.paints == paints_before,
           "a result after shutdown changed the heatmap");
}

void test_job_ticket_survives_download_boundaries()
{
    CancellableSpawn spawn;
    auto queued = spawn.ticket();
    spawn.cancel();
    auto old = spawn.run({"sleep", "30"}, queued);
    expect(old.cancelled && !old.started, "cancel before worker begins must stick to queued ticket");
    auto current = spawn.ticket();
    auto vocab = spawn.run({"true"}, current);
    expect(vocab.exited && vocab.exit_code == 0, "new job may download vocabulary");
    spawn.cancel();
    auto model = spawn.run({"sleep", "30"}, current);
    expect(model.cancelled && !model.started, "cancel between vocabulary and model must prevent second child");
    auto fresh = spawn.run({"true"}, spawn.ticket());
    expect(fresh.exited && fresh.exit_code == 0, "fresh ticket still runs");
}

void test_latest_build_replaces_pending_snapshots()
{
    BackgroundWorker worker;
    std::promise<void> entered, release, finished;
    auto gate = release.get_future().share();
    std::atomic<int> completed{0};
    worker.post([&] { entered.set_value(); gate.wait(); });
    entered.get_future().wait();
    worker.post([&] { completed.fetch_add(100); }); // stale query
    worker.post_latest([&] { completed.fetch_add(10); }); // stale snapshot
    worker.post_latest([&] { completed.fetch_add(1); finished.set_value(); });
    release.set_value();
    auto done = finished.get_future();
    expect(done.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
           "latest build should run after the download finishes");
    worker.shutdown();
    expect(completed.load() == 1, "only the newest snapshot runs");
}

} /* namespace */

int main()
{
    test_latest_build_replaces_pending_snapshots();
    test_quit_during_download();
    test_quit_during_embed();
    test_on_stop_runs_on_worker();
    test_cancel_before_lock();
    test_job_ticket_survives_download_boundaries();
    test_stale_query_colours();
    if (g_fails) {
        std::cerr << g_fails << " failure(s)\n";
        return 1;
    }
    std::cout << "lifecycle ok\n";
    return 0;
}
