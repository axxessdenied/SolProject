#include "sol/core/jobs.hpp"

#include "sol/test/test.hpp"

#include <atomic>
#include <cstdint>
#include <vector>

namespace {

using sol::core::JobCounter;
using sol::core::JobSystem;

} // namespace

SOL_TEST(jobs_parallelForCoversEveryIndexExactlyOnce)
{
    JobSystem jobs;
    std::vector<std::atomic<std::uint32_t>> hits(100'000);
    jobs.parallelFor(static_cast<std::uint32_t>(hits.size()), 1024,
                     [&hits](std::uint32_t begin, std::uint32_t end) {
                         for (std::uint32_t i = begin; i < end; ++i) {
                             hits[i].fetch_add(1, std::memory_order_relaxed);
                         }
                     });
    for (const std::atomic<std::uint32_t>& hit : hits) {
        SOL_CHECK(hit.load() == 1);
    }
}

SOL_TEST(jobs_parallelForComputesCorrectResult)
{
    JobSystem jobs;
    constexpr std::uint32_t kCount = 50'000;
    std::vector<std::uint64_t> values(kCount);
    jobs.parallelFor(kCount, 512, [&values](std::uint32_t begin, std::uint32_t end) {
        for (std::uint32_t i = begin; i < end; ++i) {
            values[i] = static_cast<std::uint64_t>(i) * 3u;
        }
    });
    std::uint64_t sum = 0;
    for (const std::uint64_t v : values) {
        sum += v;
    }
    // 3 * (0 + 1 + ... + 49999)
    SOL_CHECK(sum == 3ull * (static_cast<std::uint64_t>(kCount - 1) * kCount / 2));
}

SOL_TEST(jobs_parallelForHandlesSmallAndEmptyRanges)
{
    JobSystem jobs(2);
    std::atomic<std::uint32_t> total{0};
    jobs.parallelFor(0, 64, [&total](std::uint32_t begin, std::uint32_t end) {
        total.fetch_add(end - begin);
    });
    SOL_CHECK(total.load() == 0);
    jobs.parallelFor(5, 64, [&total](std::uint32_t begin, std::uint32_t end) {
        total.fetch_add(end - begin);
    });
    SOL_CHECK(total.load() == 5);
}

SOL_TEST(jobs_submitAndWaitRunsEveryJob)
{
    JobSystem jobs;
    constexpr std::uint32_t kJobCount = 1000;
    std::atomic<std::uint32_t> executed{0};
    JobCounter counter;
    for (std::uint32_t i = 0; i < kJobCount; ++i) {
        jobs.submit(
            [](void* user) {
                static_cast<std::atomic<std::uint32_t>*>(user)->fetch_add(
                    1, std::memory_order_relaxed);
            },
            &executed, counter);
    }
    jobs.wait(counter);
    SOL_CHECK(counter.done());
    SOL_CHECK(executed.load() == kJobCount);
}

SOL_TEST(jobs_workerCanForkAndJoinNestedWork)
{
    JobSystem jobs;
    struct Nested
    {
        JobSystem* jobs = nullptr;
        std::atomic<std::uint32_t> inner{0};
    };
    Nested nested{.jobs = &jobs};

    JobCounter outer;
    jobs.submit(
        [](void* user) {
            Nested* n = static_cast<Nested*>(user);
            // A job doing its own fork-join must not deadlock: waiting helps
            // execute queued jobs on this worker thread.
            n->jobs->parallelFor(256, 16, [n](std::uint32_t begin, std::uint32_t end) {
                n->inner.fetch_add(end - begin, std::memory_order_relaxed);
            });
        },
        &nested, outer);
    jobs.wait(outer);
    SOL_CHECK(nested.inner.load() == 256);
}
