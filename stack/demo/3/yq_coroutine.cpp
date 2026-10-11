#include "yq_coroutine.hpp"
namespace yq
{
	thread_local CoroutineCore CoroutineCore::co_root = CoroutineCore();
	thread_local CoroutineCore* CoroutineCore::co_cur_handle = &CoroutineCore::co_root;
} //namespace yq
