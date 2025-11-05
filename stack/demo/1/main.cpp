#include <print>
#include <functional>
#include <memory>
#include <cassert>

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


class CoContext {
public:
	explicit CoContext(std::function<void()> task):
		m_context{},
		m_main_context{},
		m_task {std::move(task)},
#ifdef CO_USE_UCONTEXT
		m_stack_size {64 * 1024},
		m_stack {std::make_unique<char[]>(m_stack_size)},
#endif
		m_finished {false}
	{
#ifdef CO_USE_UCONTEXT
		// 保存当前上下文到m_context
		getcontext(&m_context);
		// 设置栈空间
		m_context.uc_stack.ss_sp = m_stack.get();
		m_context.uc_stack.ss_size = m_stack_size;
		// 设置执行结束后返回的上下文
		m_context.uc_link = &m_main_context;
		// 设置入口函数, 函数无参数
		makecontext(&m_context, context_entry, 0);
#endif
	}

	~CoContext() {
#ifdef CO_USE_FIBER
		if (m_context) {
			DeleteFiber(m_context);
		}
#endif
	}

	CoContext(const CoContext&) = delete;
	CoContext& operator=(const CoContext&) = delete;
	
	void resume() {
		if (m_finished) {
			std::println("coroutine finished");
			return;
		}
#ifdef CO_USE_FIBER
		// fiber首次调用时初始化
		if (m_context == nullptr) {
			m_main_context = ConvertThreadToFiber(nullptr);
			// 如果当前线程已经是fiber, 直接获取
			if (!m_main_context) {
				m_main_context = GetCurrentFiber();
			}
			// 创建协程fiber
			m_context = CreateFiber(0, context_entry, this);
		}
#endif
		// 第一次会设置，后面都是重复设置
		m_current = this;

#ifdef CO_USE_FIBER
		SwitchToFiber(m_context);
#else
		// 保存当前上下文到m_main_context, 切换到协程上下文m_context
		swapcontext(&m_main_context, &m_context);
#endif
	}

	// 通过current访问当前协程
	static void yield() {
		// 检查是否在一个协程上下文中
		if (m_current != nullptr) {
#ifdef CO_USE_FIBER
			SwitchToFiber(m_current->m_main_context);
#else
			// 保存状态到m_current->m_context, 切换到主函数上下文
			swapcontext(&m_current->m_context, &m_current->m_main_context);
#endif
		}

		// 如果不在协程中，什么都不做
	}

	[[nodiscard]]
	auto is_finished() const noexcept -> bool {
		return m_finished;
	}

private:
#ifdef CO_USE_FIBER
	/**
	 * param param 创建Fiber时传入的参数（this指针）
	 */
	static void CALLBACK context_entry(void* param) {
		auto* co = static_cast<CoContext*>(param);
		try {
			co->m_task();
		}
		catch (...) {
			std::println("excepted exception");
			co->m_finished = true;
			throw;
		}
		co->m_finished = true;
		// 切换回主函数
		SwitchToFiber(co->m_main_context);
	}
#else
	static void context_entry() {
		m_current->m_task();
		m_current->m_finished = true;
		// 函数返回时， 使用uc_link中设置的上下文，切换回主函数
	}
#endif

private:
	static inline thread_local CoContext* m_current { nullptr };
	// 句柄在Windows下是void*, unix下是ucontext_t
	// 协程的句柄
	CoHandle m_context;
	// 主函数句柄
	CoHandle m_main_context;
	// 任务函数
	std::function<void()> m_task;
#ifdef CO_USE_UCONTEXT
	// 栈大小
	std::size_t m_stack_size;
	// 栈空间
	std::unique_ptr<char[]> m_stack;
#endif
	// 结束标记
	bool m_finished;

};

void func() {
	std::println("invoke 1");
	CoContext::yield();
	std::println("invoke 2");
	CoContext::yield();
	std::println("invoke 3");
	CoContext::yield();
	std::println("finish");
}

auto main() -> int {
	CoContext context{ func };
	std::println("main");
	while (!context.is_finished()) {
		context.resume();
	}
}