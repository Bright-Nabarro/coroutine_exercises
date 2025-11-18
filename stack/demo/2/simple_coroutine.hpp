#pragma once

#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>
#include <cassert>
#include <cstdlib>

#if defined(_WIN32)

#include <windows.h>
#define CO_USE_FIBER
using CoHandle = void*;

#elif defined(__unix__)

#include <ucontext.h>
using CoHandle = ucontext_t;
#define CO_USE_UCONTEXT

#else
#error "Unsupported platform"
#endif


class Coroutine {
private:
	// 提供给co_list使用
	Coroutine();

public:
	explicit Coroutine(std::function<void()> task, std::size_t stack_size = 64 * 1024);
	~Coroutine();
	Coroutine(const Coroutine&) = delete;
	Coroutine& operator=(const Coroutine&) = delete;
	
	void resume();

	// 通过current访问当前协程
	static void yield();

	[[nodiscard]]
	auto is_finished() const noexcept -> bool {
		return m_finished;
	}

private:
#ifdef CO_USE_FIBER
	/**
	 * param param 创建Fiber时传入的参数(this指针), 也可以用m_current访问
	 */
	static void CALLBACK context_entry(void* param);
#else
	static void context_entry();
#endif

private:
	// co_list第一个元素
	static thread_local Coroutine co_root;
	// 协程调用栈
	static thread_local std::vector<Coroutine*> co_list;
	// 当前运行的context
	static inline thread_local std::size_t co_cur_index { 0 };
	// 协程的句柄
	CoHandle m_handle;
	// 当前协程在co_list中的索引
	std::size_t m_cur_index;

	// 任务函数
	std::function<void()> m_task;
	// 栈大小
	std::size_t m_stack_size;
	// 协程是否结束
	bool m_finished { false };

#ifdef CO_USE_UCONTEXT
	// 栈空间
	std::unique_ptr<char[]> m_stack;
#endif
};

