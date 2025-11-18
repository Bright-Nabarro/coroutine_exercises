#include "simple_coroutine.hpp"

thread_local Coroutine Coroutine::co_root = Coroutine();
thread_local std::vector<Coroutine*> Coroutine::co_list =
	std::vector<Coroutine*>{&Coroutine::co_root};

Coroutine::Coroutine()
	: m_handle{}, m_cur_index{0}, m_task{}, m_stack_size{0}, m_finished{true}
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

Coroutine::Coroutine(std::function<void()> task, std::size_t stack_size)
	: m_cur_index{co_list.size()}, m_task{std::move(task)},
	  m_stack_size{stack_size}
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
	m_handle.uc_link = &co_list[m_cur_index - 1]->m_handle;
	// 设置入口函数, 函数无参数
	makecontext(&m_handle, context_entry, 0);
#endif
	co_list.push_back(this);
	co_cur_index = m_cur_index - 1;
}


Coroutine::~Coroutine() {
	if (this != &co_root)
	{
#ifdef CO_USE_FIBER
		DeleteFiber(m_handle);
#endif
		// co_list比co_root早析构，所以需要判断是否为co_root
		assert(co_list.size() >= 1);
		assert(co_list.back() == this);
		co_list.pop_back();
		assert(!co_list.empty());
		co_cur_index = m_cur_index - 1;
	}
}


void Coroutine::resume() {
	if (m_finished) {
		throw std::logic_error{"coroutine finished"};
		return;
	}
	assert(co_list[m_cur_index] == this);

	co_cur_index = m_cur_index;
#ifdef CO_USE_FIBER
	SwitchToFiber(m_handle);
#else
	// 保存当前上下文到param1, 切换到协程上下文param2
	swapcontext(&co_list[m_cur_index - 1]->m_handle, &m_handle);
#endif
}

// 通过co_cur_index访问当前协程
void Coroutine::yield() {
	// 检查是否在一个协程上下文中
	if (co_list.size() <= 1 || co_cur_index == 0) {
		throw std::logic_error{"not in coroutine or coroutine finished"};
	}

	std::size_t ori_index = co_cur_index;
	co_cur_index -= 1;
#ifdef CO_USE_FIBER
	// 切换到当前fiber上一层的fiber
	SwitchToFiber(co_list[ori_index-1]->m_handle);
#else
	// 保存当前上下文到param1, 切换到协程上下文param2
	swapcontext(&co_list[ori_index]->m_handle,
				&co_list[ori_index - 1]->m_handle);
#endif
}

#ifdef CO_USE_FIBER

/**
 * param param 创建Fiber时传入的参数(this指针), 也可以用m_current访问
 */
void CALLBACK Coroutine::context_entry(void* param) {
	auto* co_current = static_cast<Coroutine*>(param);
	assert(!co_list.empty());
	assert(co_cur_index == co_list.size() - 1);
	assert(co_list.back() == param);
	try {
		co_current->m_task();
	} catch (...) {
		co_current->m_finished = true;
		throw;
	}
	co_current->m_finished = true;
	// 切换回上一级
	SwitchToFiber(co_list[co_current->m_cur_index-1]->m_handle);
	// co_cur_index 已经在析构函数中更新
}

#else

void Coroutine::context_entry() {
	assert(co_list.size() > 1);
	assert(co_cur_index == co_list.size() - 1);
	auto* co_current = co_list.back();
	try {
		co_current->m_task();
	}
	catch (...) {
		co_current->m_finished = true;
		throw;
	}
	co_current->m_finished = true;
	// 函数返回时， 使用uc_link中设置的上下文，切换回主函数
	// co_cur_index 已经在析构函数中更新
}

#endif