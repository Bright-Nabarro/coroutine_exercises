#include <print>
#include "simple_coroutine.hpp"


auto main() -> int {
	Coroutine co1([]{
		std::println("co1 start");
		for (int i = 0; i < 5; ++i) {
			printf("co1: %d\n", i);
			Coroutine::yield();
		}
		std::println("co1 end");
	});

	while(!co1.is_finished()) {
		co1.resume();
	}
}