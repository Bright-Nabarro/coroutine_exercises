#include "yq_coroutine.hpp"
namespace yq
{
	thread_local CoroutineCore CoroutineCore::co_root = CoroutineCore();
	thread_local CoroutineCore* CoroutineCore::co_cur_handle = &CoroutineCore::co_root;

#ifdef CO_USE_FIBER
	void CoroutineCore::ensure_root_context()
	{
		auto& root = co_root;
		if (root.handle != nullptr)
		{
			return;
		}
		if (IsThreadAFiber())
		{
			root.handle = GetCurrentFiber();
			return;
		}

		auto root_handle = ConvertThreadToFiber(nullptr);
		if (!root_handle)
		{
			const DWORD error = GetLastError();
			throw std::system_error(
				static_cast<int>(error), std::system_category(), "ConvertThreadToFiber");
		}
		root.handle = root_handle;
	}
#endif
} //namespace yq
