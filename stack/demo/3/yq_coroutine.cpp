#include "yq_coroutine.hpp"
namespace yq
{
	thread_local CoroutineCore CoroutineCore::co_root = CoroutineCore();
	thread_local CoroutineCore* CoroutineCore::co_cur_handle = &CoroutineCore::co_root;

	void CoroutineCore::check_yield() const
	{
		if (CoroutineCore::co_cur_handle != this) {
			throw CoroutineException{"cannot yield from a coroutine that is not running"};
		}
		if (this == &CoroutineCore::co_root || caller == nullptr) {
			throw CoroutineException{"cannot yield from the root context"};
		}
		if (status == CoroutineStatus::Cancelling) {
			throw Cancelled{};
		}
	}

	void CoroutineCore::yield()
	{
		check_yield();
		suspend();
	}

	void CoroutineCore::suspend()
	{
		assert(CoroutineCore::co_cur_handle == this);
		CoroutineCore::co_cur_handle = caller;
		status = CoroutineStatus::Suspended;
#ifdef CO_USE_FIBER
		SwitchToFiber(caller->handle);
#elifdef CO_USE_UCONTEXT
		swapcontext(&handle, &caller->handle);
#endif
		if (status == CoroutineStatus::Cancelling) {
			throw Cancelled{};
		}
	}

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
