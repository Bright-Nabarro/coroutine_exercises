#include <print>
#include <coroutine>
#include <format>

struct PreviousAwaiter {
	bool await_ready() const noexcept {
		std::println("PreviousAwaiter await_ready");
		return false;
	}

	auto await_suspend(std::coroutine_handle<> ) const -> std::coroutine_handle<> {
		std::print("PreviousAwaiter await_suspend ");
		if (m_previous) {
			std::println("previous");
			return m_previous;
		} else {
			std::println("noop");
			return std::noop_coroutine();
		}
	}

	void await_resume() const noexcept {
		std::println("PreviousAwaiter resume");
	}

	std::coroutine_handle<> m_previous;
};


struct Promise {
	Promise():
		m_value{}
	{
		std::println("{} construct", name());
	}

	auto initial_suspend() {
		std::println("{} initial_suspend", name());
		return std::suspend_always{};
	}

	auto final_suspend() noexcept {
		std::println("{} final suspend", name());
		return PreviousAwaiter(m_previous);
	}

	void unhandled_exception() {
		std::abort();
	}

	auto yield_value(int value) {
		m_value = value;
		return std::suspend_always{};
	}

	void return_void() {
		std::println("{} return void", name());
	}

	auto get_return_object() -> std::coroutine_handle<Promise> {
		std::println("{} get return object", name());
		return std::coroutine_handle<Promise>::from_promise(*this);
	}

	auto name() -> std::string {
		return std::format("promise {}", reinterpret_cast<uint64_t>(this) % 100);
	};

	int m_value;
	std::coroutine_handle<> m_previous = nullptr;
};


struct HelloAwaiter {
	bool await_ready() const noexcept {
		std::println("HelloAwaiter await_ready");
		return false;
	}

	auto await_suspend(std::coroutine_handle<> handle) const -> std::coroutine_handle<> {
		std::println("HelloAwaiter await_suspend");
		m_handle.promise().m_previous = handle;
		return m_handle;
	}

	void await_resume() const {
		std::println("HelloAwaiter await_resume");
	}

	std::coroutine_handle<Promise> m_handle;
};

struct WorldAwaiter {
	bool await_ready() const noexcept {
		std::println("HelloAwaiter await_ready");
		return false;
	}

	void await_suspend(std::coroutine_handle<>) const {
		std::println("HelloAwaiter await_suspend");
	}

	void await_resume() const {
		std::println("HelloAwaiter await_resume");
	}
};

struct HelloTask {
	using promise_type = Promise;
	HelloTask(std::coroutine_handle<promise_type> handle):
		m_handle { handle }
	{
		std::println("HelloTask construct");
	}

	auto operator co_await() {
		std::println("HelloTask operator co_await");
		return HelloAwaiter {m_handle};
	}

	~HelloTask() {
		std::println("HelloTask destructor");
		m_handle.destroy();
	}

	std::coroutine_handle<promise_type> m_handle;
};


struct WorldTask {
	using promise_type = Promise;
	WorldTask(std::coroutine_handle<promise_type> handle):
		m_handle { handle }
	{
		std::println("WorldTask construct");
	}

	~WorldTask() {
		std::println("WorldTask destructor");
		m_handle.destroy();
	}
	std::coroutine_handle<promise_type> m_handle;
};

auto hello() -> HelloTask {
	std::println("hello");
	co_return;
}

auto world() -> WorldTask {
	std::println("world start");
	std::println("hello() start");
	co_await hello();
	std::println("hello() end");
	co_return;
}


auto main() -> int {
	auto task = world();
	std::println("start while");
	while(!task.m_handle.done()) {
		std::println("main resume");
		task.m_handle.resume();
	}
}

