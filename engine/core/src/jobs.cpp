#include "sol/core/jobs.hpp"

namespace sol::core {

JobSystem::JobSystem(std::uint32_t workerCount)
{
    if (workerCount == 0) {
        const std::uint32_t hardware = std::thread::hardware_concurrency();
        workerCount = hardware > 1 ? hardware - 1 : 1;
    }
    m_workers.reserve(workerCount);
    for (std::uint32_t i = 0; i < workerCount; ++i) {
        m_workers.emplace_back([this] { workerLoop(); });
    }
}

JobSystem::~JobSystem()
{
    {
        const std::scoped_lock lock(m_mutex);
        SOL_ASSERT(m_queue.empty()); // outstanding jobs must be waited on before teardown
        m_stop = true;
    }
    m_wake.notify_all();
    for (std::thread& worker : m_workers) {
        worker.join();
    }
}

void JobSystem::submit(JobFn fn, void* user, JobCounter& counter)
{
    SOL_ASSERT(fn != nullptr);
    counter.m_pending.fetch_add(1, std::memory_order_relaxed);
    {
        const std::scoped_lock lock(m_mutex);
        m_queue.push_back(Job{.fn = fn, .user = user, .counter = &counter});
    }
    m_wake.notify_one();
}

void JobSystem::wait(JobCounter& counter)
{
    while (!counter.done()) {
        Job job;
        if (tryPop(job)) {
            execute(job);
        } else {
            std::this_thread::yield();
        }
    }
}

bool JobSystem::tryPop(Job& job)
{
    const std::scoped_lock lock(m_mutex);
    if (m_queue.empty()) {
        return false;
    }
    job = m_queue.back();
    m_queue.pop_back();
    return true;
}

void JobSystem::execute(const Job& job)
{
    job.fn(job.user);
    job.counter->m_pending.fetch_sub(1, std::memory_order_release);
}

void JobSystem::workerLoop()
{
    for (;;) {
        Job job;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [this] { return m_stop || !m_queue.empty(); });
            if (m_stop && m_queue.empty()) {
                return;
            }
            job = m_queue.back();
            m_queue.pop_back();
        }
        execute(job);
    }
}

} // namespace sol::core
