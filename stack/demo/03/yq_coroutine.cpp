#include "yq_coroutine.hpp"

namespace yq {
thread_local VarCoroutine<> BaseCoroutine::co_root = VarCoroutine<>();
thread_local std::vector<BaseCoroutine*> BaseCoroutine::co_list =
	std::vector<BaseCoroutine*>{&BaseCoroutine::co_root};

} // namespace yq
