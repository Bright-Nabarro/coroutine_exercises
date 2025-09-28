#include <coroutine>
#include <deque>
#include <print>
#include <chrono>
#include <deque>
using namespace std::chrono_literals;

struct PreviousAwaiter {
	PreviousAwaiter(std::coroutine_handle<> handle): m_previous{ handle } {
	}

	auto await_ready()  const noexcept -> bool {
		return false;
	}

	auto await_suspend(std::coroutine_handle<>) const noexcept
		-> std::coroutine_handle<>
	{
		if (m_previous) {
			return m_previous;
		} else {
			return std::noop_coroutine();
		}
	}

	void await_resume() const noexcept {}

	std::coroutine_handle<> m_previous;
};


template<typename PromiseType>
struct BasePromise {
	auto initial_suspend() {
		return std::suspend_always{};
	}

	auto final_suspend() noexcept {
		return PreviousAwaiter(m_previous);
	}

	void unhandled_exception() {
		throw;
	}

	auto get_return_object() -> std::coroutine_handle<PromiseType> {
		return std::coroutine_handle<PromiseType>::from_promise(*static_cast<PromiseType*>(this));
	};

	std::coroutine_handle<> m_previous;
};


template<typename Ty = void>
struct Promise: public BasePromise<Promise<Ty>> {
	Promise(): m_value{} {
	}

	template<typename TyRef>
	auto yield_value(TyRef&& value) {
		m_value = std::forward<TyRef>(value);
		return std::suspend_always{};
	}

	template<typename TyRef>
	void return_value(TyRef&& value) {
		m_value = std::forward<TyRef>(value);
	}

	Ty m_value;
};


template <>
struct Promise<void> : public BasePromise<Promise<void>> {
	void return_void() {}
};


template<typename Ty = void>
struct Task {
	using promise_type = Promise<Ty>;

	Task(std::coroutine_handle<promise_type> coroutine) noexcept:
		m_coroutine { coroutine } {}
	
	~Task() {
		m_coroutine.destroy();
	}

	struct Awaiter {
		auto await_ready() const noexcept -> bool {
			return false;
		}
		
		auto await_suspend(std::coroutine_handle<> coroutine) const noexcept
			-> std::coroutine_handle<promise_type>
		{
			m_coroutine.promise().m_previous = coroutine;
		}

		auto await_resume() const -> Ty {
			return m_coroutine.promise().m_value;
		}
		
		std::coroutine_handle<promise_type> m_coroutine;
	};

	auto operator co_await() const noexcept {
		return Awaiter { m_coroutine };
	}

	operator std::coroutine_handle<>() const noexcept {
		return m_coroutine;
	}

	std::coroutine_handle<promise_type> m_coroutine;
};


class Loop {
public:
	std::deque<std::coroutine_handle<>> m_ready_queue;
	struct TimerEntry {
		std::chrono::system_clock::time_point expire_tp;
		std::coroutine_handle<> coroutine;
	};

private:
};
