#include <coroutine>
#include <print>

struct GeneratorVerbose {
	struct promise_type;
	using handle_type = std::coroutine_handle<promise_type>;

	handle_type coro;

	GeneratorVerbose() {
		std::println("\tGeneratorVerbose::GeneratorVerbose()");
	}

	~GeneratorVerbose() {
		std::println("\tGeneratorVerbose::~GeneratorVerbose()");
		if (coro)
			coro.destroy();
	}

	int getNextValue() {
		std::println("\tGeneratorVerbose::getNextValue()");
		coro.resume();
		return coro.promise().current_value;
	}

	struct promise_type {
		promise_type(int, GeneratorVerbose& gen_verbose) {
			std::println("\tpromise_type::promise_type()");
			gen_verbose.coro = handle_type::from_promise(*this);
		}

		~promise_type() {
			std::println("\tpromise_type::~promise_type()");
		}

		std::suspend_always initial_suspend() {
			std::println("\tpromise_type::initial_suspend()");
			return {};
		}

		std::suspend_always final_suspend() noexcept {
			std::println("\tpromise_type::final_suspend()");
			return {};
		}

		auto get_return_object() {
			std::println("\tpromise_type::get_return_object()");
			return;
		}

		std::suspend_always yield_value(int value) {
			std::println("\tpromise_type::yield_value()");
			current_value = value;
			return {};
		}

		void return_void() {
			std::println("\tpromise_type::return_void()");
		}

		void unhandled_exception() {
			std::println("\tpromise_type::unhandled_exception()");
			std::terminate();
		}

		int current_value;
	};
};


struct Generator {

	struct promise_type;
	using handle_type = std::coroutine_handle<promise_type>;

	handle_type coro;

	~Generator() {
		if (coro)
			coro.destroy();
	}

	int getNextValue() {
		coro.resume();
		return coro.promise().current_value;
	}
	struct promise_type {
		promise_type(int, Generator& gen) {
			gen.coro = handle_type::from_promise(*this);
		}

		std::suspend_always initial_suspend() {
			return {};
		}
		std::suspend_always final_suspend() noexcept {
			return {};
		}

		auto get_return_object() {}

		std::suspend_always yield_value(int value) {
			current_value = value;
			return {};
		}
		void return_void() {}
		void unhandled_exception() {
			std::exit(1);
		}

		int current_value;
	};
};


template<>
struct std::coroutine_traits<void, int, GeneratorVerbose&> {
	using promise_type = GeneratorVerbose::promise_type;
};


template<>
struct std::coroutine_traits<void, int, Generator&> {
	using promise_type = Generator::promise_type;
};


template <typename CoroutineInterface>
void getNext(int start, CoroutineInterface&) {
	if constexpr (std::is_same_v<CoroutineInterface, GeneratorVerbose>) {
		std::println("\tgetNext<GeneratorVerbose> called with start={}", start);
	} 	

	auto value = start;
	while(true) {
		co_yield value;
		value += 1;
	}

	if constexpr (std::is_same_v<CoroutineInterface, GeneratorVerbose>) {
		std::println("\tgetNext<GeneratorVerbose> ended");
	}
}

int main() {
	// 非典型写法，通过promise_type将handle塞给Generator
	std::println("=== GeneratorVerbose ===");
	{
		GeneratorVerbose gen_verbose;
		getNext(0, gen_verbose);
		for (int i = 0; i < 5; ++i) {
			auto value = gen_verbose.getNextValue();
			std::println("main got value: {}", value);
		}
	}

	std::println("\n=== Generator ===");
	{
		Generator gen;
		getNext(0, gen);
		for (int i = 0; i < 5; ++i) {
			auto value = gen.getNextValue();
			std::println("main got value: {}", value);
		}
	}

	std::println("\n=== done ===");
}