#pragma once
#include "coroutine_exception.hpp"
#include "result_storage.hpp"
#include <exception>
#include <functional>
#include <memory>
#include <system_error>
#include <utility>
#include <cassert>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#define CO_USE_FIBER
namespace yq {
using CoHandle = void*;
}

#elif defined(__unix__)

#include <ucontext.h>
namespace yq {
using CoHandle = ucontext_t;
}
#define CO_USE_UCONTEXT

#else
#error "Unsuppored platform"
#endif

#ifdef CO_USE_FIBER
#define CONTEXT_ENTRY_PROPERTY CALLBACK
#define CONTEXT_ENTRY_PARAM(param) void* param
using ContextEntryFunc = void (CALLBACK*)(void*);
#elif defined(__unix__)
#define CONTEXT_ENTRY_PROPERTY  
#define CONTEXT_ENTRY_PARAM(param)
using ContextEntryFunc = void(*)();
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
		// co_root 的 thread_local 构造只初始化数据，避免系统错误在这里抛出。
		// ucontext 的根上下文由 swapcontext 保存。
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
#ifdef CO_USE_FIBER
	static void ensure_root_context();
#endif
	void yield();

	CoHandle handle {};
	CoroutineStatus status {};
	CoroutineCore* caller { nullptr };
	std::size_t stack_size { 0 };
	std::exception_ptr excepted { nullptr };
	// libc++没有move_only_function的实现，使用function替代
	std::function<void()> task{};
#ifdef CO_USE_UCONTEXT
	// 栈空间
	std::unique_ptr<char[]> stack{};
#endif

private:
	void check_yield() const;
	void suspend();
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

// 调度接口不依赖结果类型；结果由 Coroutine<R> 自己保存和取出。
class CoroutineBase
{
public:
	CoroutineBase(const CoroutineBase&) = delete;
	CoroutineBase& operator=(const CoroutineBase&) = delete;
	virtual ~CoroutineBase() = default;

	virtual void resume() = 0;
	virtual void yield() = 0;
	virtual void cancel() = 0;
	virtual bool is_finished() const noexcept = 0;
	virtual CoroutineStatus status() const noexcept = 0;

protected:
	CoroutineBase() = default;
};

template<typename R = void>
class Coroutine : public CoroutineBase
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
		assert(CoroutineCore::co_cur_handle != nullptr);
#ifdef CO_USE_FIBER
		assert(core->handle == nullptr);
		CoroutineCore::ensure_root_context();
		core->handle = CreateFiber(m_handle->core.stack_size, Coroutine<R>::context_entry, this);
		if (!core->handle)
		{
			const DWORD error = GetLastError();
			throw std::system_error(
				static_cast<int>(error), std::system_category(), "CreateFiber");
		}
#elifdef CO_USE_UCONTEXT
		assert(stack_size > 0);
		core->stack = std::make_unique<char[]>(core->stack_size);
		// 当前上下文作为初始化模版
		getcontext(&core->handle);
		// 设置栈空间
		core->handle.uc_stack.ss_size = core->stack_size;
		core->handle.uc_stack.ss_sp = core->stack.get();
		core->handle.uc_link = nullptr;
		//设置入口函数
		makecontext(&core->handle, context_entry, 0);
		
#endif
		core->status = CoroutineStatus::Ready;
	}

	Coroutine(const Coroutine&) = delete;
	Coroutine& operator=(const Coroutine&) = delete;

	~Coroutine() override
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

	// 只恢复执行，不消费暂停值或最终值。
	void resume() override
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
#elifdef CO_USE_UCONTEXT
		
		// 保存当前上下文到param1, 切换到协程上下文param2
		swapcontext(&m_handle->core.caller->handle, &m_handle->core.handle);
#endif
		check_exception();
	}

	// 取出已有的暂停值或最终值，取出后清空；非 void 的结果没有待取值时抛出使用错误。
	R result()
	{
		check_exception();
		const auto s = status();
		if (s != CoroutineStatus::Suspended && s != CoroutineStatus::Finished) {
			throw CoroutineException{"cannot get a result before the coroutine pauses or finishes"};
		}
		if constexpr (!std::is_void_v<R>) {
			if (!m_handle->ret.has_value()) {
				throw CoroutineException{"coroutine has no result"};
			}
		}
		return m_handle->ret.forward();
	}

	template<typename R_Ref>
	void yield(R_Ref&& value) requires (!std::is_void_v<R>)
	{
		m_handle->core.check_yield();
		m_handle->ret.emplace(std::forward<R_Ref>(value));
		m_handle->core.suspend();
	}

	// 任意结果类型都允许无值暂停，供 sleep 等调度操作使用。
	void yield() override
	{
		m_handle->core.yield();
	}

	void cancel() override
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
#ifdef CO_USE_FIBER
		SwitchToFiber(m_handle->core.handle);
#elifdef CO_USE_UCONTEXT
		swapcontext(&m_handle->core.caller->handle, &m_handle->core.handle);
#endif
	}

	constexpr bool is_finished() const noexcept override
	{
		return status() == CoroutineStatus::Finished;
	}

	constexpr CoroutineStatus status() const noexcept override
	{
		return m_handle->core.status;
	}

private:
	void check_exception() const {
		if (m_handle->core.excepted) {
			std::rethrow_exception(m_handle->core.excepted);
		}
	}

	// param为当前函数的this指针
	static void CONTEXT_ENTRY_PROPERTY context_entry(CONTEXT_ENTRY_PARAM(param))
	{
		auto* core = CoroutineCore::co_cur_handle;
		assert(core->caller != nullptr);
		//当前执行的是在刚resume后，所以调用栈最新
		assert(CoroutineCore::co_cur_handle == core);
		assert(core->status == CoroutineStatus::Running);
		try {
			core->task();
		} catch(Cancelled) {
			//pass
		}catch(...) {
			core->excepted = std::current_exception();
		}
		core->status = CoroutineStatus::Finished;

		CoroutineCore::co_cur_handle = core->caller;
		assert(core->caller);
#ifdef CO_USE_FIBER
		(void)param;
		SwitchToFiber(core->caller->handle);
#elifdef CO_USE_UCONTEXT
		// caller可能是动态的, uc_link无法固定，所以手动切回caller
		setcontext(&core->caller->handle);
#endif
		assert(false && "terminate");
	}

private:
	std::unique_ptr<CoroutineState<R>> m_handle;
};

namespace co //放一些相关的常用全局函数
{

inline void yield()
{
	CoroutineCore::co_cur_handle->yield();
}

inline void resume(CoroutineBase& co_handle)
{
	co_handle.resume();
}

template<typename R>
inline R resume(Coroutine<R>& co_handle)
{
	co_handle.resume();
	return co_handle.result();
}

template<typename R, typename R_Ref>
void yield(Coroutine<R>& co_handle, R_Ref&& value)
{
	co_handle.yield(std::forward<R_Ref>(value));
}

inline void yield(CoroutineBase& co_handle)
{
	co_handle.yield();
}

} //namespace yq::co

} 	//namespace yq
	

#undef CONTEXT_ENTRY_PROPERTY
#undef CONTEXT_ENTRY_PARAM

