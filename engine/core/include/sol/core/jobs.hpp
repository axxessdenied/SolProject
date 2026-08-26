#pragma once

#include "sol/core/assert.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace sol::core {

// Fork-join completion counter. Reaches zero when every job submitted
// against it has finished. Must outlive those jobs - the standard shape is a
// stack-local counter waited on before scope exit.
class JobCounter
{
public:
    [[nodiscard]] bool done() const { return m_pending.load(std::memory_order_acquire) == 0; }

private:
    friend class JobSystem;
    std::atomic<std::uint32_t> m_pending{0};
};

using JobFn = void (*)(void* user);

// Worker-thread pool for fork-join parallelism. Jobs are plain function
// pointer + context records - no per-job allocation, no exceptions. The
// queue is a mutex-guarded LIFO; upgrade to per-worker stealing deques only
// if profiling ever shows contention (see engine plan 2.1).
class JobSystem
{
public:
    // workerCount 0 picks hardware_concurrency - 1 (min 1).
    explicit JobSystem(std::uint32_t workerCount = 0);
    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;
    ~JobSystem();

    [[nodiscard]] std::uint32_t workerCount() const { return static_cast<std::uint32_t>(m_workers.size()); }

    // Enqueue fn(user). Everything user points at must stay valid until a
    // wait on the counter returns.
    void submit(JobFn fn, void* user, JobCounter& counter);

    // Execute queued jobs on the calling thread until the counter is done.
    void wait(JobCounter& counter);

    // Fork-join over [0, count): fn(begin, end) is called for chunks of at
    // most chunkSize indices, on the workers and the calling thread, with
    // dynamic load balancing. Returns when the whole range is processed.
    template <typename Fn>
    void parallelFor(std::uint32_t count, std::uint32_t chunkSize, Fn&& fn)
    {
        SOL_ASSERT(chunkSize > 0);
        if (count == 0) {
            return;
        }
        if (count <= chunkSize) {
            fn(0u, count);
            return;
        }

        struct Context
        {
            Fn* fn = nullptr;
            std::uint32_t count = 0;
            std::uint32_t chunkSize = 0;
            std::atomic<std::uint32_t> next{0};
        };

        Context context{.fn = &fn, .count = count, .chunkSize = chunkSize};

        const auto run = [](void* user) {
            Context* ctx = static_cast<Context*>(user);
            for (;;) {
                const std::uint32_t begin = ctx->next.fetch_add(ctx->chunkSize, std::memory_order_relaxed);
                if (begin >= ctx->count) {
                    break;
                }
                (*ctx->fn)(begin, std::min(begin + ctx->chunkSize, ctx->count));
            }
        };

        const std::uint32_t chunks = (count + chunkSize - 1) / chunkSize;
        const std::uint32_t helpers = std::min(workerCount(), chunks - 1);
        JobCounter counter;
        for (std::uint32_t i = 0; i < helpers; ++i) {
            submit(run, &context, counter);
        }
        run(&context);
        wait(counter);
    }

private:
    struct Job
    {
        JobFn fn = nullptr;
        void* user = nullptr;
        JobCounter* counter = nullptr;
    };

    [[nodiscard]] bool tryPop(Job& job);
    static void execute(const Job& job);
    void workerLoop();

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::vector<Job> m_queue;
    std::vector<std::thread> m_workers;
    bool m_stop = false;
};

} // namespace sol::core
