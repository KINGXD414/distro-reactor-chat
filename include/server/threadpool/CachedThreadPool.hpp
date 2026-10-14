#ifndef CHAT_CACHED_THREAD_POOL_HPP
#define CHAT_CACHED_THREAD_POOL_HPP
#include "TaskExecutor.hpp"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <algorithm>

namespace pool {
// Safe adaptation of the external cached-pool idea. Queue pressure grows
// workers; a full queue waits instead of executing a DB task on the IO thread.
class CachedThreadPool : public TaskExecutor {
public:
    CachedThreadPool(std::size_t capacity, std::size_t core, std::size_t max,
                     unsigned keepAliveSeconds = 5)
        : m_capacity(capacity), m_core(std::max<std::size_t>(1, core)),
          m_max(std::max(m_core, max)), m_keepAlive(keepAliveSeconds),
          m_running(true), m_active(0), m_live(0) {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (std::size_t i = 0; i < m_core; ++i) startWorkerLocked();
    }
    ~CachedThreadPool() override { Stop(); }

    bool submitTask(TaskType task) override {
        if (!task) return false;
        std::unique_lock<std::mutex> lock(m_mutex);
        while (m_running && m_queue.size() >= m_capacity) m_notFull.wait(lock);
        if (!m_running) return false;
        m_queue.emplace_back(std::move(task));
        if (m_active >= m_live && m_live < m_max)
            startWorkerLocked();
        lock.unlock();
        m_notEmpty.notify_one();
        return true;
    }
    std::size_t queueSize() const override {
        std::lock_guard<std::mutex> lock(m_mutex); return m_queue.size();
    }
    std::size_t queueCapacity() const override { return m_capacity; }
    void Stop() override {
        std::call_once(m_stopOnce, [this] {
            { std::lock_guard<std::mutex> lock(m_mutex); m_running = false; }
            m_notEmpty.notify_all(); m_notFull.notify_all();
            for (auto &worker : m_workers) if (worker.joinable()) worker.join();
            m_workers.clear();
        });
    }
private:
    void startWorkerLocked() { ++m_live; m_workers.emplace_back([this] { runWorker(); }); }
    void runWorker() {
        for (;;) {
            TaskType task;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                const bool ready = m_notEmpty.wait_for(lock,
                    std::chrono::seconds(m_keepAlive),
                    [this] { return !m_running || !m_queue.empty(); });
                if (!ready && m_running && m_live > m_core) { --m_live; return; }
                if (!m_running && m_queue.empty()) return;
                if (m_queue.empty()) continue;
                task = std::move(m_queue.front()); m_queue.pop_front(); ++m_active;
                m_notFull.notify_one();
            }
            try { task(); } catch (...) {}
            { std::lock_guard<std::mutex> lock(m_mutex); --m_active; }
        }
    }
    const std::size_t m_capacity, m_core, m_max;
    const unsigned m_keepAlive;
    mutable std::mutex m_mutex;
    std::condition_variable m_notEmpty, m_notFull;
    std::deque<TaskType> m_queue;
    std::vector<std::thread> m_workers;
    std::atomic<bool> m_running;
    std::size_t m_active;
    std::size_t m_live;
    std::once_flag m_stopOnce;
};
}
#endif
