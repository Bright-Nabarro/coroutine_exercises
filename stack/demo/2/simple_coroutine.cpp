#include "simple_coroutine.hpp"

thread_local VarCoroutine<> BaseCoroutine::co_root = VarCoroutine<>();
thread_local std::vector<BaseCoroutine*> BaseCoroutine::co_list =
	std::vector<BaseCoroutine*>{&BaseCoroutine::co_root};

