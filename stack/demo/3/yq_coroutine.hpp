#pragma once
#include "coroutine_exception.hpp"
#include "result_storage.hpp"
#include <exception>
#include <functional>
#include <memory>
#include <utility>
#include <cassert>

#if defined(_WIN32)

#include <windows.h>
#define CO_USE_FIBER
namespace yq {
using CoHandle = void*;
}

#else

#error "Unsuppored platform"

#endif

#ifdef CO_USE_FIBER
#define CONTEXT_ENTRY_PROPERTY CALLBACK
#define CONTEXT_ENTRY_PARAM(param) VOID* param
using ContenxtEntryFunc = VOID (CALLBACK*)(VOID*);
#endif

#define CO_DEFAULT_STACK_SIZE 2*1024*1024


namespace yq
{

template<typename T>
class Coroutine;

enum class CoroutineStatus
{
	Uninitailized = 0,
	Ready,
	Running,
	Suspended,
	Cancelling,
	Finished,
};

struct CoroutineCore
{
	CoroutineCore()
	{
		//给co_root构建使用
		//assert()
#ifdef CO_USE_FIBER
		handle = ConvertThreadToFiber(nullptr);
		if (!handle)
		{
			handle = GetCurrentFiber();
		}
#endif
		status = CoroutineStatus::Ready;
	}
	CoroutineCore(std::size_t stack_size_):
		stack_size { stack_size_ }
	{
	}
	template<typename T>
	friend class Coroutine;

	// 为了使最外层（主函数）不创建协程环境情况下可以resume
	static thread_local CoroutineCore co_root;
	static thread_local CoroutineCore* co_cur_handle;
	CoHandle handle {};
	CoroutineStatus status {};
	CoroutineCore* caller { nullptr };
	std::size_t stack_size { 0 };
	std::exception_ptr excepted { nullptr };
	std::function<void()> task{};
};

template<typename R>
struct CoroutineState
{
	CoroutineState(){}
	CoroutineState(std::size_t stack_size):
		core{stack_size}
	{}
	CoroutineCore core {};
	// 对void有特化，不能直接optional
	ResultStorage<R> ret;
};


template<typename R = void>
class Coroutine
{
public:
	template<typename Func, typename... Args>
		requires (!std::is_same_v<std::remove_cvref_t<Func>, Coroutine>
				&& std::invocable<Func, Args...>)
	explicit Coroutine(Func&& func, Args&&... args)
		: Coroutine<R>(CO_DEFAULT_STACK_SIZE,
			std::forward<Func>(func), std::forward<Args>(args)...
		)
	{
	}

	template<typename Func, typename... Args>
		// 防止复制构造等出现
		requires (!std::is_same_v<std::remove_cvref_t<Func>, Coroutine>
				&& std::invocable<Func, Args...>)
	Coroutine(std::size_t stack_size, Func&& func, Args&&... args)
		: m_handle(std::make_unique<CoroutineState<R>>(stack_size))
	{
		CoroutineState<R>* state = m_handle.get();
		// 函数内部进行了std::move所以mutable
		state->core.task = [state,
			f = std::forward<Func>(func),
			...xs = std::forward<Args>(args)]() mutable {
			if constexpr (std::is_void_v<R>) {
				std::invoke(std::move(f), std::move(xs)...);
				state->ret.emplace();
			} else {
				// 先执行任务得到值，再写入 ret；任务期间可以 yield。
				state->ret.emplace(std::invoke(std::move(f), std::move(xs)...));
			}
		};

		CoroutineCore* core = &state->core;
		// 对于不是co_root构造的，handle没有赋值
		assert(core->handle == nullptr);
		assert(CoroutineCore::co_cur_handle != nullptr);
#ifdef CO_USE_FIBER
		core->handle = CreateFiber(m_handle->core.stack_size, Coroutine<R>::context_entry, this);
#endif
		core->status = CoroutineStatus::Ready;
	}

	Coroutine(const Coroutine&) = delete;
	Coroutine& operator=(const Coroutine&) = delete;

	~Coroutine()
	{
		if (this != static_cast<void*>(&CoroutineCore::co_root))
		{
			if (!is_finished())
			{
				cancel();
			}
#ifdef CO_USE_FIBER
			DeleteFiber(m_handle->core.handle);
#endif
			m_handle = nullptr;
		}
	}

	R resume()
	{
		check_exception();
		const auto s = status();
		if (s != CoroutineStatus::Ready && s != CoroutineStatus::Suspended) {
			throw CoroutineException{"coroutine is not resumable"};
		}
		if (is_finished()) {
			throw CoroutineException{"cannot resume a finished coroutine"};
		}

		if (CoroutineCore::co_cur_handle == &m_handle->core)
		{
			throw CoroutineException{"cannot resume the running coroutine"};
		}

		m_handle->core.caller = CoroutineCore::co_cur_handle;
		CoroutineCore::co_cur_handle = &m_handle->core;
		assert(CoroutineCore::co_cur_handle == &m_handle->core);
		m_handle->core.status = CoroutineStatus::Running;
#ifdef CO_USE_FIBER
		SwitchToFiber(m_handle->core.handle);
#endif
		check_exception();
		return m_handle->ret.forward();
	}

	template<typename R_Ref>
	void yield(R_Ref&& value) requires (!std::is_void_v<R>)
	{
		check_yield();
		m_handle->ret.emplace(std::forward<R_Ref>(value));
		suspend();
	}

	void yield() requires std::is_void_v<R>
	{
		check_yield();
		suspend();
	}

	void cancel()
	{
		if (status() == CoroutineStatus::Running)
		{
			throw CoroutineException{"cannot cancel a running coroutine"};
		}
		if (status() == CoroutineStatus::Finished)
		{
			throw CoroutineException{"cannot cancel a finished coroutine"};
		}
		if (status() == CoroutineStatus::Ready)
		{
			m_handle->core.status = CoroutineStatus::Finished;
			return;
		}

		m_handle->core.status = CoroutineStatus::Cancelling;
		m_handle->core.caller = CoroutineCore::co_cur_handle;
		CoroutineCore::co_cur_handle = &m_handle->core;
		SwitchToFiber(m_handle->core.handle);
	}

	constexpr bool is_finished() const noexcept
	{
		return status() == CoroutineStatus::Finished;
	}

	constexpr CoroutineStatus status() const noexcept
	{
		return m_handle->core.status;
	}

private:
	void check_yield() const
	{
		// yield时不在当前协程上下文
		if (CoroutineCore::co_cur_handle != &m_handle->core) {
			throw CoroutineException{"cannot yield from a coroutine that is not running"};
		}
		if (status() == CoroutineStatus::Cancelling)
		{
			throw Cancelled{};
		}
	}

	void suspend() 
	{
		assert(CoroutineCore::co_cur_handle == &m_handle->core);
		CoroutineCore::co_cur_handle = m_handle->core.caller;
		m_handle->core.status = CoroutineStatus::Suspended;
#ifdef CO_USE_FIBER
		SwitchToFiber(m_handle->core.caller->handle);
#endif
		if (status() == CoroutineStatus::Cancelling)
		{
			throw Cancelled{};
		}
	}

	void check_exception() const {
		if (m_handle->core.excepted) {
			std::rethrow_exception(m_handle->core.excepted);
		}
	}

	// param为当前函数的this指针
	static void CONTEXT_ENTRY_PROPERTY context_entry(CONTEXT_ENTRY_PARAM(param))
	{
#ifdef CO_USE_FIBER
		auto* co_current = static_cast<Coroutine<R>*>(param);
		CoroutineState<R>* state = co_current->m_handle.get();
		auto& core = state->core;
		assert(core.caller != nullptr);
		//当前执行的是在刚resume后，所以调用栈最新
		assert(CoroutineCore::co_cur_handle == &core);
		assert(core.status == CoroutineStatus::Running);
		try {
			core.task();
		} catch(Cancelled) {
			//pass
		}catch(...) {
			core.excepted = std::current_exception();
		}
		core.status = CoroutineStatus::Finished;

		CoroutineCore::co_cur_handle = core.caller;
		assert(core.caller);
		SwitchToFiber(core.caller->handle);
#endif
	}

private:
	std::unique_ptr<CoroutineState<R>> m_handle;
};

namespace co //放一些相关的常用全局函数
{

template<typename R>
R resume(Coroutine<R>& co_handle)
{
	return co_handle.resume();
}

template<typename R, typename R_Ref>
void yield(Coroutine<R>& co_handle, R_Ref&& value)
{
	co_handle.yield(std::forward<R_Ref>(value));
}

template<typename R>
void yield(Coroutine<R>& co_handle)
{
	co_handle.yield();
}

} //namespace yq::co

} 	//namespace yq
	

#undef CONTEXT_ENTRY_PROPERTY
#undef CONTEXT_ENTRY_PARAM

