#include <print>
#include <functional>
#include <memory>

#ifdef _MSC_VER
#include <windows.h>

// ============================================
// 简单协程类 - Windows Fiber版本
// ============================================
class SimpleCoroutine {
private:
    // ========== 成员变量 ==========
    
    /**
     * fiber_: 协程的Fiber句柄
     * - Fiber是Windows提供的轻量级线程
     * - 每个Fiber有独立的栈空间和执行上下文
     * - nullptr表示尚未创建
     */
    LPVOID fiber_{nullptr};
    
    /**
     * main_fiber_: 主函数的Fiber句柄
     * - 保存调用resume()的线程/Fiber的上下文
     * - 用于从协程返回到主函数
     * - yield()时会切换回这个Fiber
     */
    LPVOID main_fiber_{nullptr};
    
    /**
     * task_: 协程要执行的任务函数
     * - std::function可以存储任何可调用对象（函数、lambda等）
     * - 协程启动后会执行这个函数
     * - 使用std::move避免不必要的拷贝
     */
    std::function<void()> task_;
    
    /**
     * finished_: 协程是否执行完毕的标志
     * - true: 任务已执行完，不能再resume
     * - false: 任务未完成或正在执行
     */
    bool finished_{false};
    
    /**
     * current_: 当前正在运行的协程指针（线程局部变量）
     * - thread_local: 每个线程有独立的副本
     * - 用于在静态函数yield()中访问当前协程
     */
    static inline thread_local SimpleCoroutine* current_{nullptr};

public:
    // ========== 构造函数 ==========
    
    /**
     * 构造函数：创建一个新的协程对象
     * @param func 要在协程中执行的任务函数
     * 
     * explicit: 防止隐式类型转换
     * std::move(func): 移动语义，避免拷贝函数对象
     */
    explicit SimpleCoroutine(std::function<void()> func) : task_{std::move(func)} {}
    
    // ========== 析构函数 ==========
    
    /**
     * 析构函数：清理协程资源
     * - 如果fiber_不为空，删除Fiber对象
     * - Windows会自动回收Fiber的栈空间
     */
    ~SimpleCoroutine() {
        if (fiber_) DeleteFiber(fiber_);
    }
    
    // ========== 禁止拷贝 ==========
    
    /**
     * 删除拷贝构造和拷贝赋值
     * - 协程对象持有系统资源（Fiber句柄）
     * - 拷贝会导致资源管理混乱
     * - 只允许移动语义（如果需要的话）
     */
    SimpleCoroutine(const SimpleCoroutine&) = delete;
    SimpleCoroutine& operator=(const SimpleCoroutine&) = delete;
    
    // ========== 核心方法：resume ==========
    
    /**
     * resume(): 启动或恢复协程执行
     * 
     * 执行流程：
     * 1. 检查协程是否已结束
     * 2. 首次调用时初始化Fiber
     * 3. 切换到协程上下文执行
     * 4. 协程yield或结束后返回这里
     */
    void resume() {
        // --- 步骤1：检查协程状态 ---
        if (finished_) {
            std::println("协程已结束");
            return;  // 已结束的协程不能再恢复
        }
        
        // --- 步骤2：首次调用时的初始化 ---
        if (!fiber_) {
            // 如果线程已经是fiber，直接获取当前句柄
            main_fiber_ = ConvertThreadToFiber(nullptr);
            if (!main_fiber_) {
                main_fiber_ = GetCurrentFiber();
            }
            
            /**
             * CreateFiber: 创建新的Fiber
             * - 参数1 (0): 栈大小，0表示使用默认大小（通常1MB）
             * - 参数2 (fiber_entry): Fiber的入口函数
             * - 参数3 (this): 传递给入口函数的参数（当前协程对象指针）
             */
            fiber_ = CreateFiber(0, fiber_entry, this);
        }
        
        // --- 步骤3：设置当前协程并切换上下文 ---
        // 相当于previous
        // 压入返回地址
        current_ = this;
        
        /**
         * SwitchToFiber: 切换到协程Fiber执行
         * - 保存当前执行状态（寄存器、栈指针等）
         * - 恢复fiber_的执行状态
         * - 跳转到协程代码继续执行
         * 
         * 注意：这个函数会"阻塞"直到：
         * - 协程调用yield()切换回来
         * - 或协程执行完毕
         */
        SwitchToFiber(fiber_);
        
        // --- 步骤4：协程返回后继续执行这里 ---
        // 此时协程已经yield或执行完毕
    }
    
    // ========== 核心方法：yield ==========
    
    /**
     * yield(): 暂停协程，返回到调用resume()的地方
     * 
     * 这是一个静态方法，因为：
     * - 在协程内部调用，不需要协程对象指针
     * - 通过current_访问当前协程
     */
    static void yield() {
        // --- 检查是否在协程上下文中 ---
        if (current_ && current_->main_fiber_) {
            /**
             * SwitchToFiber: 切换回主Fiber
             * - 保存协程的执行状态
             * - 恢复main_fiber_的执行状态
             * - 返回到resume()函数中SwitchToFiber的下一行
             * 
             * 当再次resume()时，会从这里的下一行继续执行
             */
            SwitchToFiber(current_->main_fiber_);
        }
        // 如果不在协程中调用yield，什么都不做
    }
    
    // ========== 查询方法 ==========
    
    /**
     * is_finished(): 查询协程是否执行完毕
     * 
     * [[nodiscard]]: C++17属性，提示调用者不应忽略返回值
     * noexcept: 保证不抛异常
     */
    [[nodiscard]] bool is_finished() const noexcept { return finished_; }

private:
    // ========== Fiber入口函数 ==========
    
    /**
     * fiber_entry: 协程Fiber的入口函数
     * @param param 创建Fiber时传入的参数（this指针）
     * 
     * CALLBACK: Windows调用约定
     * 
     * 执行流程：
     * 1. 将参数转换为协程对象指针
     * 2. 执行用户任务
     * 3. 标记协程完成
     * 4. 返回主Fiber
     */
    static VOID CALLBACK fiber_entry(LPVOID param) {
        // --- 步骤1：获取协程对象 ---
        /**
         * 将void*参数转换回SimpleCoroutine*
         * - CreateFiber时传入的this指针
         */
        auto* co = static_cast<SimpleCoroutine*>(param);
        
        // --- 步骤2：执行用户任务 ---
        /**
         * 调用用户提供的任务函数
         * - 这里会执行协程的实际逻辑
         * - 可能包含多次yield()调用
         */
        co->task_();
        
        // --- 步骤3：标记完成 ---
        /**
         * 任务执行完毕，设置完成标志
         * - 之后调用resume()会直接返回
         */
        co->finished_ = true;
        
        // --- 步骤4：返回主Fiber ---
        /**
         * 最后一次切换回主函数
         * - 这次切换后，协程不会再被执行
         * - resume()会检测到finished_为true
         */
        SwitchToFiber(co->main_fiber_);
    }
};

#else
// ============================================
// GCC/Clang版本 - 使用ucontext
// ============================================
#include <ucontext.h>
#include <cstdlib>

class SimpleCoroutine {
private:
    // ========== 成员变量 ==========
    
    /**
     * co_context_: 协程的执行上下文
     * - ucontext_t是POSIX标准的上下文结构
     * - 保存CPU寄存器、栈指针、指令指针等
     * - 用于保存和恢复协程的执行状态
     */
    ucontext_t co_context_{};
    
    /**
     * main_context_: 主函数的执行上下文
     * - 保存调用resume()时的执行状态
     * - yield()时切换回这个上下文
     */
    ucontext_t main_context_{};
    
    /**
     * stack_: 协程的栈空间
     * - std::unique_ptr自动管理内存
     * - char[]数组作为栈空间
     * - 协程的局部变量、函数调用都在这个栈上
     */
    std::unique_ptr<char[]> stack_;
    
    /**
     * task_: 协程要执行的任务函数
     */
    std::function<void()> task_;
    
    /**
     * finished_: 协程是否执行完毕
     */
    bool finished_{false};
    
    /**
     * current_: 当前正在运行的协程（线程局部）
     */
    static inline thread_local SimpleCoroutine* current_{nullptr};

public:
    // ========== 构造函数 ==========
    
    /**
     * 构造函数：创建协程并初始化上下文
     * @param func 任务函数
     * @param stack_size 栈大小（默认64KB）
     */
    explicit SimpleCoroutine(std::function<void()> func, size_t stack_size = 64 * 1024) 
        : stack_{std::make_unique<char[]>(stack_size)}, task_{std::move(func)} {
        
        // --- 步骤1：获取当前上下文 ---
        /**
         * getcontext: 获取当前执行上下文
         * - 将当前CPU状态保存到co_context_
         * - 作为makecontext的基础
         */
        getcontext(&co_context_);
        
        // --- 步骤2：设置协程栈 ---
        /**
         * 配置协程使用的栈空间
         * - ss_sp: 栈的起始地址
         * - ss_size: 栈的大小
         */
        co_context_.uc_stack.ss_sp = stack_.get();
        co_context_.uc_stack.ss_size = stack_size;
        
        // --- 步骤3：设置协程结束后的行为 ---
        /**
         * uc_link: 协程执行完毕后要切换到的上下文
         * - 设置为main_context_，执行完自动返回主函数
         * - 如果为nullptr，执行完会退出线程
         */
        co_context_.uc_link = &main_context_;
        
        // --- 步骤4：设置协程入口函数 ---
        /**
         * makecontext: 修改上下文，设置入口函数
         * - 参数1: 要修改的上下文
         * - 参数2: 入口函数指针
         * - 参数3: 入口函数的参数个数（0表示无参数）
         */
        makecontext(&co_context_, coroutine_entry, 0);
    }
    
    // ========== 禁止拷贝 ==========
    SimpleCoroutine(const SimpleCoroutine&) = delete;
    SimpleCoroutine& operator=(const SimpleCoroutine&) = delete;
    
    // ========== 核心方法：resume ==========
    
    /**
     * resume(): 启动或恢复协程
     */
    void resume() {
        // --- 检查状态 ---
        if (finished_) {
            std::println("协程已结束");
            return;
        }
        
        // --- 设置当前协程 ---
        current_ = this;
        
        // --- 切换上下文 ---
        /**
         * swapcontext: 保存当前上下文并切换到新上下文
         * - 参数1: 保存当前上下文的位置（main_context_）
         * - 参数2: 要切换到的上下文（co_context_）
         * 
         * 执行效果：
         * 1. 保存当前CPU状态到main_context_
         * 2. 从co_context_恢复CPU状态
         * 3. 跳转到协程代码执行
         * 
         * 当协程yield时，会从这里的下一行继续执行
         */
        swapcontext(&main_context_, &co_context_);
    }
    
    // ========== 核心方法：yield ==========
    
    /**
     * yield(): 暂停协程
     */
    static void yield() {
        if (current_) {
            /**
             * swapcontext: 从协程切换回主函数
             * - 保存协程状态到co_context_
             * - 恢复main_context_的状态
             * - 返回到resume()中swapcontext的下一行
             * 
             * 下次resume时，会从这里的下一行继续执行
             */
            swapcontext(&current_->co_context_, &current_->main_context_);
        }
    }
    
    // ========== 查询方法 ==========
    [[nodiscard]] bool is_finished() const noexcept { return finished_; }

private:
    // ========== 协程入口函数 ==========
    
    /**
     * coroutine_entry: 协程的入口函数
     * - 必须是静态函数（makecontext要求）
     * - 通过current_访问协程对象
     */
    static void coroutine_entry() {
        // --- 执行用户任务 ---
        /**
         * 执行用户提供的任务函数
         * - current_在resume()中已设置
         */
        current_->task_();
        
        // --- 标记完成 ---
        current_->finished_ = true;
        
        // --- 自动返回 ---
        /**
         * 函数返回时，由于uc_link设置为main_context_
         * 会自动切换回主函数，无需手动swapcontext
         */
    }
};

#endif

// ============================================
// 测试代码
// ============================================

/**
 * coroutine_task: 在协程中执行的任务
 * - 演示协程的暂停和恢复
 */
void coroutine_task() {
    std::println("协程开始执行");
    
    for (int i : {1, 2, 3}) {
        std::println("协程执行第 {} 步", i);
        
        /**
         * yield: 暂停协程，返回主函数
         * - 保存当前执行位置
         * - 下次resume会从这里继续
         */
        SimpleCoroutine::yield();
        
        /**
         * resume后从这里继续执行
         */
        std::println("协程恢复执行");
    }
    
    std::println("协程执行完毕");
    // 函数返回，协程结束
}

/**
 * main: 主函数，演示协程的使用
 */
int main() {
    std::println("=== 简单协程示例 ===\n");
    
    // --- 创建协程 ---
    /**
     * 使用花括号初始化（C++11统一初始化）
     * - 传入任务函数coroutine_task
     */
    SimpleCoroutine co{coroutine_task};
    
    // --- 第一次resume ---
    /**
     * 首次启动协程
     * - 执行到第一个yield()
     * - 打印"协程执行第 1 步"后返回
     */
    std::println("主函数：首次启动协程");
    co.resume();
    
    std::println("\n主函数：做一些其他工作...\n");
    
    // --- 第二次resume ---
    /**
     * 恢复协程执行
     * - 从第一个yield()后继续
     * - 执行到第二个yield()
     */
    std::println("主函数：第二次恢复协程");
    co.resume();
    
    std::println("\n主函数：继续工作...\n");
    
    // --- 第三次resume ---
    std::println("主函数：第三次恢复协程");
    co.resume();
    
    // --- 第四次resume ---
    /**
     * 最后一次恢复
     * - 协程执行完毕
     * - finished_被设置为true
     */
    std::println("\n主函数：最后一次恢复协程");
    co.resume();
    
    // --- 检查状态 ---
    /**
     * 格式化输出：{}会被后面的参数替换
     * - 三元运算符：条件 ? 真值 : 假值
     */
    std::println("\n协程是否结束：{}", co.is_finished() ? "是" : "否");
    
    return 0;
}
