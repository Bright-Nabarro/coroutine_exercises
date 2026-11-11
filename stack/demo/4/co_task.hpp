#pragma once

#include "coroutine.hpp"
#include <chrono>
#include <memory>
#include <utility>

namespace yq {

class CoLoop;

class CoTaskBase : public std::enable_shared_from_this<CoTaskBase> {
    friend class CoLoop;

public:
    CoTaskBase(const CoTaskBase&) = delete;
    CoTaskBase& operator=(const CoTaskBase&) = delete;
    virtual ~CoTaskBase() = default;

    bool is_finished() noexcept { return base_coroutine().is_finished(); }

    void cancel() {
        if (is_finished())
            return;
        const auto status = base_coroutine().status();
        if (status != CoroutineStatus::Ready && status != CoroutineStatus::Suspended)
            throw CoroutineException{"task is not cancellable"};
        base_coroutine().cancel();
        m_cancelled = true;
        m_waiting_on.reset();
    }

protected:
    CoTaskBase() = default;
    void wait();
    bool was_cancelled() const noexcept { return m_cancelled; }
    virtual CoroutineBase& base_coroutine() noexcept = 0;

private:
    using Clock = std::chrono::steady_clock;
    Clock::time_point m_wake_at = (Clock::time_point::min)();
    std::shared_ptr<CoTaskBase> m_waiting_on;
    bool m_cancelled = false;
};

template<class R>
class CoTask final : public CoTaskBase {
public:
    template<class Func, class... Args>
    explicit CoTask(Func&& work, Args&&... args)
        : m_coroutine(std::forward<Func>(work), std::forward<Args>(args)...) {}

    // 只取最终结果；取出后清空，支持 void 和只能移动的值。
    R result() {
        if (!is_finished())
            throw CoroutineException{"task has not finished"};
        if (was_cancelled())
            throw CoroutineException{"task was cancelled"};
        if (m_result_taken)
            throw CoroutineException{"task result was already consumed"};
        m_result_taken = true;
        return m_coroutine.result();
    }

    R await() {
        // 循环移除已结束的任务后，仍保证本次取值期间任务存活。
        auto keep_alive = shared_from_this();
        wait();
        return result();
    }

private:
    CoroutineBase& base_coroutine() noexcept override { return m_coroutine; }
    Coroutine<R> m_coroutine;
    bool m_result_taken = false;
};

} // namespace yq
