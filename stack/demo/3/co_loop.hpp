#pragma once

#include "yq_coroutine.hpp"
#include "co_task.hpp"
#include <algorithm>
#include <chrono>
#include <list>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>

namespace yq {

// 每个线程一个循环；任务主动 sleep() 后，才轮到其他任务执行。
class CoLoop {
public:
    static CoLoop& instance() {
        // 根上下文先初始化，在线程退出时晚于任务列表析构。
        auto& root = CoroutineCore::co_root;
        (void)root;
        static thread_local CoLoop loop;
        return loop;
    }

    CoLoop(const CoLoop&) = delete;
    CoLoop& operator=(const CoLoop&) = delete;

    template<typename Func, typename... Args>
    auto spawn(Func&& work, Args&&... args) {
        // 与 Coroutine 保存参数后的调用方式一致；引用返回也按值保存。
        using R = std::remove_cvref_t<std::invoke_result_t<
            std::decay_t<Func>, std::decay_t<Args>...>>;
        auto coroutine = std::make_shared<Coroutine<R>>(
            std::forward<Func>(work), std::forward<Args>(args)...);
		return ensure_future<R>(coroutine);
    }

	template<typename R>
	auto ensure_future(std::shared_ptr<Coroutine<R>> coroutine) {
		auto task = std::make_shared<CoTask<R>>(std::move(coroutine));
		m_tasks.push_back(task);
		return task;
	}

    void sleep(std::chrono::milliseconds delay) {
        if (m_current == nullptr) {
            throw CoroutineException{"sleep must be called by a task in the loop"};
        }
        m_current->wake_at = Clock::now() + delay;
        m_current->base_coroutine()->yield();
        // 下一次被循环 resume() 时，从这里继续。
    }

    void run() {
        if (m_running) {
            throw CoroutineException{"the coroutine loop is already running"};
        }
        m_running = true;
        try {
            while (!m_tasks.empty()) {
                auto next_wakeup = (Clock::time_point::max)();
                for (auto it = m_tasks.begin(); it != m_tasks.end();) {
                    // 调用方可能已通过句柄取消任务。
                    if ((*it)->base_coroutine()->is_finished()) {
                        it = m_tasks.erase(it);
                        continue;
                    }
                    if ((*it)->wake_at <= Clock::now()) {
                        m_current = &**it;
                        // 静态类型是 CoroutineBase，只恢复
                        m_current->base_coroutine()->resume();
                        m_current = nullptr;

                        if ((*it)->base_coroutine()->is_finished()) {
                            it = m_tasks.erase(it);
                            continue;
                        }
                    }
                    next_wakeup = std::min(next_wakeup, (*it)->wake_at);
                    ++it;
                }

                // 全部任务都在等待时，线程才休眠，避免空转。
                if (!m_tasks.empty() && next_wakeup > Clock::now()) {
                    std::this_thread::sleep_until(next_wakeup);
                }
            }
        } catch (...) {
            m_current = nullptr;
            // 调用方可能仍持有句柄，清空列表不会立即析构协程，必须先取消。
            for (auto& task : m_tasks) {
                if (!task->base_coroutine()->is_finished()) {
                    task->base_coroutine()->cancel();
                }
            }
            m_tasks.clear();
            m_running = false;
            throw;
        }
        m_running = false;
    }

private:
    using Clock = std::chrono::steady_clock;

    CoLoop() = default;

    std::list<std::shared_ptr<CoTaskBase>> m_tasks;
    CoTaskBase* m_current = nullptr;
    bool m_running = false;
};

} // namespace yq
