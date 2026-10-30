#include "co_loop.hpp"
#include <algorithm>
#include <thread>

namespace yq {

CoLoop& CoLoop::instance() {
    // 先初始化根上下文，使其在线程退出时晚于循环析构。
    auto& root = CoroutineCore::co_root;
    (void)root;
    static thread_local CoLoop loop;
    return loop;
}

void CoTaskBase::wait() {
    CoLoop::instance().wait(shared_from_this());
}

void CoLoop::sleep(std::chrono::milliseconds delay) {
    if (m_current == nullptr)
        throw CoroutineException{"sleep must be called by a task in the loop"};
    auto* current = m_current;
    const auto old_wakeup = current->m_wake_at;
    current->m_wake_at = Clock::now() + delay;
    try {
        // 使用成员入口确认暂停的是循环当前任务，而不是其手动恢复的内层协程。
        current->base_coroutine().yield();
    } catch (...) {
        current->m_wake_at = old_wakeup;
        throw;
    }
}

void CoLoop::wait(const std::shared_ptr<CoTaskBase>& task) {
    if (task->is_finished())
        return;
    if (m_current == nullptr)
        throw CoroutineException{"await of an unfinished task must be called inside the loop"};
    if (std::find(m_tasks.begin(), m_tasks.end(), task) == m_tasks.end())
        throw CoroutineException{"awaited task does not belong to this loop"};

    // 包括自己等待自己，以及 A 等 B、B 又等 A。
    for (auto dependency = task; dependency; dependency = dependency->m_waiting_on) {
        if (dependency.get() == m_current)
            throw CoroutineException{"task await would create a cycle"};
    }

    auto* current = m_current;
    const auto old_wakeup = current->m_wake_at;
    current->m_waiting_on = task;
    current->m_wake_at = (Clock::time_point::min)();
    try {
        current->base_coroutine().yield();
    } catch (...) {
        current->m_waiting_on.reset();
        current->m_wake_at = old_wakeup;
        throw;
    }
    current->m_waiting_on.reset();
}

void CoLoop::run() {
    if (m_running)
        throw CoroutineException{"the coroutine loop is already running"};
    if (CoroutineCore::co_cur_handle != &CoroutineCore::co_root)
        throw CoroutineException{"run must be called from the root context"};
    m_running = true;
    try {
        while (!m_tasks.empty()) {
            for (auto it = m_tasks.begin(); it != m_tasks.end();) {
                auto& task = *it;
                if (task->is_finished()) {
                    it = m_tasks.erase(it);
                    continue;
                }
                const bool waiting = task->m_waiting_on && !task->m_waiting_on->is_finished();
                if (!waiting && task->m_wake_at <= Clock::now()) {
                    m_current = task.get();
                    task->base_coroutine().resume(); // 不消费暂停值或最终值。
                    m_current = nullptr;
                    if (task->is_finished()) {
                        it = m_tasks.erase(it);
                        continue;
                    }
                }
                ++it;
            }

            // 本轮后才计算：后面的任务可能刚结束，唤醒了前面的等待者。
            if (!m_tasks.empty()) {
                auto next_wakeup = (Clock::time_point::max)();
                for (const auto& task : m_tasks) {
                    if (!task->m_waiting_on || task->m_waiting_on->is_finished())
                        next_wakeup = std::min(next_wakeup, task->m_wake_at);
                }
                if (next_wakeup == (Clock::time_point::max)())
                    throw CoroutineException{"no task can make progress"};
                if (next_wakeup > Clock::now())
                    std::this_thread::sleep_until(next_wakeup);
            }
        }
    } catch (...) {
        m_current = nullptr;
        // 外部可能仍持有任务句柄，所以必须主动取消，不能只 clear()。
        while (!m_tasks.empty()) {
            auto task = m_tasks.front();
            m_tasks.pop_front();
            task->cancel();
        }
        m_running = false;
        throw;
    }
    m_running = false;
}

} // namespace yq
