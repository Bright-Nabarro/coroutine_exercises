#include <coroutine>
#include <print>
#include <thread>
#include <chrono>

struct MySuspendNever {
	MySuspendNever(std::chrono::duration<double, std::milli> sleep):
	sleep_duration {sleep} {}

	std::chrono::duration<double, std::milli> sleep_duration;

	bool await_ready() const noexcept {
		std::println("\tMySuspendNever::await_ready()");
		std::this_thread::sleep_for(sleep_duration);
		return true;
	}

	void await_suspend(std::coroutine_handle<>) const noexcept {
		std::println("\tMySuspendNever::await_suspend()");
	}

	void await_resume() const noexcept {
		std::println("\tMySuspendNever::await_resume()");
	}
};


struct Job {
	struct promise_type;
	using handle_type = std::coroutine_handle<promise_type>;
	handle_type coro;
	Job(handle_type h): coro(h) {}
	~Job() {
		if (coro) coro.destroy();
	}

	void start() {
		if (coro) coro.resume();
	}

	struct promise_type {
		auto get_return_object() {
			return Job{handle_type::from_promise(*this)};
		}

		std::suspend_always initial_suspend() {
			std::println("\tpromise_type::initial_suspend()");
			return {};
		}

		std::suspend_always final_suspend() noexcept {
			std::println("\tpromise_type::final_suspend()");
			return {};
		}

		void return_void() {}

		void unhandled_exception() {
			std::println("Unhandled exception in coroutine");
			std::terminate();
		}

		auto await_transform(std::chrono::duration<double, std::milli> sleep) {
			return MySuspendNever{sleep};
		}
	};
};

Job prepare_job() {
	using namespace std::chrono_literals;
	co_await 0.5ms;
}

int main() {
	std::print("Before job...\n");

	auto start = std::chrono::high_resolution_clock::now();
	auto job = prepare_job();
	job.start();
	auto end = std::chrono::high_resolution_clock::now();
	auto duration = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();

	std::print("Job prepared and started in {} milliseconds.\n", duration);
	std::print("After job...\n");
}