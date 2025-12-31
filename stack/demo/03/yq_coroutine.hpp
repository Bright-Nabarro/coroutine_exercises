#pragma once

#include <cassert>
#include <cstdlib>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <vector>

#if defined(_WIN32)

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
#error "Unsupported platform"
#endif

namespace yq
{

enum class CoState{
	uninitialized,
	ready,
	running,
	suspended,
	waiting,
	finished,
	error,
};


template <typename... Args>
class VarCoroutine;

class BaseCoroutine {
	template<typename ...Args>
	friend class VarCoroutine;
	friend class CoStateGuard;
public:
	virtual ~BaseCoroutine() {};
	virtual void resume() = 0;
	virtual void call_task() = 0;
	[[nodiscard]]
	auto get_state() const -> CoState {
		check_exception();
		return m_state;
	}

	virtual void check_exception() const {
		if (m_excepted) {
			std::rethrow_exception(m_excepted);
		}
	}

protected:
	void switch_state(CoState state) noexcept {
		m_state = state;
	}

	// co_list第一个元素
	static thread_local VarCoroutine<> co_root;
	// 协程调用栈
	static thread_local std::vector<BaseCoroutine*> co_list;
	// 当前运行的context
	static inline thread_local std::size_t co_cur_index{0};

	// 协程的句柄
	CoHandle m_handle{};
	// 协程是否结束
	CoState m_state {CoState::uninitialized};
	std::exception_ptr m_excepted { nullptr };
};


class CoStateGuard {
public:
	CoStateGuard(BaseCoroutine* co, CoState final, bool excpet = false):
		m_co {co}, m_final_state {final}, m_except {excpet}
	{
	}

	~CoStateGuard() {
		if (!m_except) {
			m_co->switch_state(m_final_state);
		}
	}

	CoStateGuard(const CoStateGuard&) = delete;
	CoStateGuard& operator=(const CoStateGuard&) = delete;
	void set_except() {
		m_except = true;
	}

private:
	BaseCoroutine* m_co;
	CoState m_final_state;
	bool m_except;
};


template <typename... Args>
class VarCoroutine : public BaseCoroutine {
	friend BaseCoroutine;

private:
	// 提供给co_list使用
	VarCoroutine():
		BaseCoroutine(),
		m_cur_index{0}, m_stack_size{0}, m_task{}
#ifdef CO_USE_UCONTEXT
		  ,
		  m_stack{nullptr}
#endif
	{
		// 对于ucontext, 不在这里初始化. swap时会接受上下文
#ifdef CO_USE_FIBER
		m_handle = ConvertThreadToFiber(nullptr);
		if (!m_handle) {
			m_handle = GetCurrentFiber();
		}
#endif
	}

public:
	template <typename... ArgsRef>
	VarCoroutine(std::size_t stack_size, std::function<void(Args...)> task,
			  ArgsRef&&... args)
		: BaseCoroutine(), m_cur_index{co_list.size()}, m_stack_size{stack_size},
		  m_task{std::move(task)},
		  m_func_args{std::forward<ArgsRef>(args)...}
#ifdef CO_USE_UCONTEXT
		  ,
		  m_stack{std::make_unique<char[]>(m_stack_size)}
#endif
	{
		assert(co_list.size() >= 1);
#ifdef CO_USE_FIBER
		m_handle = CreateFiber(m_stack_size, context_entry, this);
#else
		// 当前上下文作为初始化模版
		getcontext(&m_handle);
		// 设置栈空间
		m_handle.uc_stack.ss_size = m_stack_size;
		m_handle.uc_stack.ss_sp = m_stack.get();
		// 设置执行结束后返回的上下文
		assert(m_cur_index > 0);
		m_handle.uc_link = &co_list[m_cur_index-1]->m_handle;
		// 设置入口函数, 函数无参数
		makecontext(&m_handle, context_entry, 0);
#endif
		co_list.push_back(this);
		co_cur_index = m_cur_index - 1;

		// 初始化完成，协程变为ready状态
		switch_state(CoState::ready);
	}

	template <typename... ArgsRef>
	VarCoroutine(std::function<void(Args...)> task, 
			  ArgsRef&&... args):
		VarCoroutine(2 * 1024 * 1024, task, std::forward<ArgsRef>(args)...) {}

	~VarCoroutine() {
		// co_list比co_root早析构，所以需要判断是否为co_root
		if (this != static_cast<void*>(&co_root)) {
#ifdef CO_USE_FIBER
			DeleteFiber(m_handle);
#endif
			assert(co_list.size() >= 1);
			assert(co_list.back() == this);
			co_list.pop_back();
			assert(!co_list.empty());
			co_cur_index = m_cur_index - 1;
		}
	}

	VarCoroutine(const VarCoroutine&) = delete;
	VarCoroutine& operator=(const VarCoroutine&) = delete;

	void resume() override {
		check_exception();
		if (m_state != CoState::suspended) {
			throw std::logic_error{"coroutine state excepted"};
			return;
		}
		assert(co_list[m_cur_index] == this);

		co_cur_index = m_cur_index;
#ifdef CO_USE_FIBER
		SwitchToFiber(m_handle);
#else
		// 保存当前上下文到param1, 切换到协程上下文param2
		swapcontext(&co_list[m_cur_index-1]->m_handle, &m_handle);
#endif
	}

	// 通过current访问当前协程
	static void yield() {
		// 检查是否在一个协程上下文中
		if (co_list.size() <= 1 || co_cur_index == 0) {
			throw std::logic_error{"not in coroutine or coroutine finished"};
		}

		std::size_t ori_index = co_cur_index;
		co_cur_index -= 1;
#ifdef CO_USE_FIBER
		// 切换到当前fiber上一层的fiber
		SwitchToFiber(co_list[ori_index - 1]->m_handle);
#else
		// 保存当前上下文到param1, 切换到协程上下文param2
		swapcontext(&co_list[ori_index]->m_handle,
					&co_list[ori_index - 1]->m_handle);
#endif
	}

private:

	virtual void call_task() override {
		std::apply(m_task, m_func_args);
	}

#ifdef CO_USE_FIBER
	/**
	 * param param 创建Fiber时传入的参数(this指针), 也可以用m_current访问
	 */
	static void CALLBACK context_entry(void* param) {
		auto* co_current = static_cast<BaseCoroutine*>(param);
		assert(!co_list.empty());
		assert(co_cur_index == co_list.size() - 1);
		assert(co_list.back() == param);
		try {
			co_current->call_task();
		} catch (...) {
			co_current->m_excepted = std::current_exception();
			co_current->m_finished = true;
		}
		co_current->m_finished = true;
		// 切换回上一级
		SwitchToFiber(co_list[co_current->co_cur_index - 1]->m_handle);
		// co_cur_index 已经在析构函数中更新
	}

#else
	static void context_entry(){
		assert(co_list.size() > 1);
		assert(co_cur_index == co_list.size() - 1);
		auto* co_current = co_list.back();
		CoStateGuard guard {co_current, CoState::finished};
		co_current->switch_state(CoState::running);
		try {
			co_current->call_task();
		} catch (...) {
			co_current->m_excepted = std::current_exception();
			co_current->switch_state(CoState::error);
			guard.set_except();
		}
		// 函数返回时， 使用uc_link中设置的上下文，切换回主函数
		// co_cur_index 已经在析构函数中更新
	}

#endif

private:
	// 当前协程在co_list中的索引
	std::size_t m_cur_index;
	// 栈大小
	std::size_t m_stack_size;
	// 任务函数
	std::function<void(Args...)> m_task;
	// 回调函数参数
	std::tuple<Args...> m_func_args;
#ifdef CO_USE_UCONTEXT
	// 栈空间
	std::unique_ptr<char[]> m_stack;
#endif
};

using Coroutine = VarCoroutine<>;

} //namespace yq