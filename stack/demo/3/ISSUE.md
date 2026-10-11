# demo/3 当前问题

`main.cpp` 使用简单检查和执行轨迹，不依赖测试框架。失败会打印 `[FAIL]`，最后返回非零退出码。

## 运行结果

2026-10-10 使用本机 clang64 构建并运行：

- 现有 CMake 构建：11 项通过，1 项失败。
- 额外以 `-O2 -DNDEBUG -Wall -Wextra -Werror` 构建：11 项通过，1 项失败。用户调用检查不依赖 `assert`。
- 两个版本均只有问题 1 的复现测试失败，程序正常报告后以退出码 1 结束。
- 协程实现没有为测试修改；下面标记为静态审查的问题没有通过故障注入或危险操作验证。

在仓库根目录运行：

```sh
cmake --build ./build/clang --target stack_demo_3
./build/clang/stack/demo/3/stack_demo_3.exe
```

## 1. 销毁暂停协程不会析构任务栈上的局部对象

状态：已有运行时回归测试，名称为 `ISSUE-1: destroying a paused task must clean up locals`。修复前应失败，不能为了全绿删掉检查或把失败当作通过。

```cpp
bool destroyed = false;
{
    Coroutine<void> co([&] {
        TaskLocal local{destroyed};
        co.yield();
    });
    co.resume();
} // 直接 DeleteFiber；local 的析构没有执行。
```

当前析构函数只删除 Fiber 的栈，没有展开 C++ 调用栈。任务栈上的资源对象可能因此泄漏。测试只使用析构标志，不分配真实堆资源来制造泄漏。

可选处理：

- 规定已启动的协程必须执行结束后才能销毁；未启动的对象仍然允许销毁。
- 提供显式 `finish()`，反复恢复并丢弃结果，直到任务自然结束。任务如果无限暂停，这个操作也无法结束。
- 设计协作式取消：恢复暂停点后抛出内部取消异常，让任务栈正常展开。清理用的恢复操作不能调用 `ret.forward()`，因为取消不会生成返回值；任务也不能吞掉取消异常后无限执行。

目前没有实现自动完成、取消或未完成销毁的限制。完成任务和任务抛出异常时的局部对象析构，另有正常测试覆盖。

这项失败测试检验的是“直接销毁暂停协程时自动清理”的能力。如果最终决定禁止这种销毁，应把它改为验证相应契约；不要求为了通过测试一定实现自动取消。

## 2. Windows Fiber API 的失败没有处理

状态：静态审查发现，没有进行故障注入测试。

- `CreateFiber` 返回空指针时，构造仍然成功，之后会用空 handle 调用 `SwitchToFiber`。
- `ConvertThreadToFiber` 失败后无条件调用 `GetCurrentFiber`，错误地把所有失败都当作“线程已经是 Fiber”。应先用 `IsThreadAFiber` 判断，再选择获取当前 Fiber 或转换线程，并处理真正的失败。
- 系统资源不足与用户违反接口约定不同，宜与 `CoroutineException` 区分。
- `co_root` 是具有动态初始化的非局部 `thread_local` 对象。若直接在其初始化中抛异常，异常会导致 `std::terminate`，不能简单照搬普通构造函数的失败回抛策略。

参考：[CreateFiber](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-createfiber)、[ConvertThreadToFiber](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-convertthreadtofiber)、[thread_local 动态初始化](https://eel.is/c++draft/basic.start.dynamic)。

## 3. 构造约束与保存后的调用类型不一致

状态：静态审查发现。以下反例不放入可执行测试，因为会阻止整个测试程序编译。

```cpp
int number = 1;
Coroutine<void> co([](int& value) { ++value; }, number);
```

`std::invocable<Func, Args...>` 按原始引用类型检查，可以接受上面的调用。但 init-capture 会保存值，实际通过 `std::move(xs)` 传入的是右值，无法绑定到 `int&`。

两个构造函数应按 `std::decay_t<Func>` 和 `std::decay_t<Args>...` 检查实际调用能力。需要引用时由用户显式传 `std::ref(number)`；当前正常测试覆盖值参数、`std::ref`、成员函数指针和指定栈大小。

## 4. 不支持恢复仍在调用链中的外层协程

状态：按当前设计暂不加入调用链遍历；属于用户必须遵守的限制。测试不执行此类操作。

```text
root -> outer.resume() -> inner.resume() -> outer.resume()
```

当前只拒绝恢复自己，不能拒绝重新进入正在等待内层返回的外层。用户必须禁止这种重入，否则会覆盖 `caller` 并破坏恢复顺序。

当前允许的情况是：一个协程先暂停返回外层，之后在没有活动调用链冲突时，由根部再次恢复。`caller changes from outer to root` 测试覆盖这个合法场景。

## 5. 销毁当前执行的协程没有保护

状态：静态审查发现。测试不执行，因为 Windows 删除当前 Fiber 会直接退出线程，测试报告可能无法完成。

析构前需要明确禁止销毁当前运行的协程，且析构函数默认不能通过普通异常回抛来报告错误。应确定采用契约约束、断言或终止策略。

析构里 `this != &CoroutineCore::co_root` 的判断不能保护这种情况：`co_root` 是 `CoroutineCore`，不是 `Coroutine<R>` 对象。

参考：[DeleteFiber](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-deletefiber)。

## 6. 当前任务包装只能保存可复制的 callable

状态：当前兼容性限制。

clang64 所用 libc++ 没有实现 `std::move_only_function`，当前使用 `std::function<void()>`。因此捕获 `unique_ptr` 等只能移动的构造参数或 callable 会导致编译失败。

这不影响返回值和暂停值为 `unique_ptr`；`move-only results and invalid value yield` 测试覆盖这个区别。若将来需要线程构造函数式的 move-only 参数支持，需要替换任务包装。

## 当前测试覆盖

- 构造不执行任务，未启动对象可销毁。
- 普通函数中的跨函数暂停，检查完整执行顺序。
- 多个暂停值和最终返回值，暂停状态与结束状态。
- 内层结束但尚未析构时，外层暂停回根部。
- 创建顺序与恢复顺序不同，以及每次恢复重新记录 caller。
- 构造值参数、引用参数、成员函数、指定栈大小。
- 只能移动的暂停值与返回值，非法 yield 不消耗其参数。
- 根部 yield、暂停对象 yield、另一个对象 yield、恢复自己、恢复已结束对象。
- 任务异常的原始类型、异常前后的执行顺序、局部对象展开和根部恢复。
- 正常完成的局部对象析构，以及问题 1 的失败复现。
