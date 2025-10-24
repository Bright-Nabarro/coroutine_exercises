#include <print>
#include <coroutine>

template<typename PromiseType>
struct BasePromise {
	auto initial_suspend() {
		return std::suspend_always{};
	}

	auto final_suspend() noexcept {
		return std::suspend_always{};
	}

	void unhandled_exception() {
		throw;
	}

	auto get_return_object() -> std::coroutine_handle<PromiseType> {
		return std::coroutine_handle<PromiseType>::from_promise(*static_cast<PromiseType*>(this));
	};
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


template<typename TaskType, typename PromiseType>
struct BaseTask {
	BaseTask(std::coroutine_handle<PromiseType> handle):
		m_handle { handle }
	{}

	auto done() const -> bool {
		return m_handle.done();
	}

	void resume() const {
		m_handle.resume();
	}

	~BaseTask() {
		m_handle.destroy();
	}

	std::coroutine_handle<PromiseType> m_handle;
};


template <typename Ty = void>
struct Task: public BaseTask<Task<Ty>, Promise<Ty>> {
	using promise_type = Promise<Ty>;

	Task(std::coroutine_handle<promise_type> handle):
		BaseTask<Task<Ty>, Promise<Ty>> { handle }
	{}
	
	auto get_value() const -> const Ty& {
		return this->m_handle.promise().m_value;
	}
};


template <>
struct Task<void>: public BaseTask<Task<void>, Promise<void>> {
	using promise_type = Promise<void>;
	Task(std::coroutine_handle<promise_type> handle):
		BaseTask<Task<void>, Promise<void>> { handle }
	{}
};


auto hello() -> Task<double> {
	std::println("hello start");
	co_yield 1.1;
	co_yield 2.2;
	std::println("hello end");
	co_return 3.3;
}

auto world() -> Task<void> {
	std::println("world");
	co_return;
}


auto main() -> int {
	auto t1 = hello();
	auto t2 = world();

	while(!t1.done()) {
		std::println("t1 resume");
		t1.resume();
	}

	while(!t2.done()) {
		std::println("t2 resume");
		t2.resume();
	}
}
