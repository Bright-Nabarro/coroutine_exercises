#pragma once

#include "co_task.hpp"
#include <chrono>
#include <list>
#include <memory>
#include <type_traits>
#include <utility>

namespace yq {

class CoLoop {
    friend class CoTaskBase;

public:
    static CoLoop& instance();

    CoLoop(const CoLoop&) = delete;
    CoLoop& operator=(const CoLoop&) = delete;

    template<class Func, class... Args>
    auto spawn(Func&& work, Args&&... args) {
        using R = std::remove_cvref_t<std::invoke_result_t<
            std::decay_t<Func>, std::decay_t<Args>...>>;
        auto task = std::make_shared<CoTask<R>>(
            std::forward<Func>(work), std::forward<Args>(args)...);
        m_tasks.push_back(task);
        return task;
    }

    void sleep(std::chrono::milliseconds delay);
    void run();

private:
    CoLoop() = default;
    void wait(const std::shared_ptr<CoTaskBase>& task);

    using Clock = std::chrono::steady_clock;
    std::list<std::shared_ptr<CoTaskBase>> m_tasks;
    CoTaskBase* m_current = nullptr;
    bool m_running = false;
};

namespace co {

static auto& syncloop { yq::CoLoop::instance() };

inline void sleep(std::chrono::milliseconds delay) {
    syncloop.sleep(delay);
}

template<class Func, class... Args>
inline auto await(Func&& work, Args&&... args) {
	auto task = syncloop.spawn(std::forward<Func>(work), std::forward<Args>(args)...);
	return task->await();
}


} // namespace co
} // namespace yq
