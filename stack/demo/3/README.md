# 协程循环例子

`co_loop.hpp` 通过 `CoroutineBase` 调度不同结果类型的 `Coroutine<R>`。
`CoLoop::instance()` 返回当前线程的 `thread_local` 单例。

```cpp
using namespace std::chrono_literals;
auto& loop = yq::CoLoop::instance();
auto task = loop.spawn([&loop](int number) {
    // 做一些工作……
    loop.sleep(100ms);
    // 唤醒后继续工作……
    return number * 2;
}, 21);
loop.run();
int result = task->coroutine()->result(); // 42；task 是 shared_ptr<CoTask<int>>。
```

- `spawn(f, args...)` 按保存后的函数和参数类型推导 `R`，构造 `Coroutine<R>`，等 `run()` 才执行。
- 循环内部保存 `shared_ptr<CoTaskBase>`，通过 `base_coroutine()` 调度；调用方拿到 `shared_ptr<CoTask<R>>`，通过 `coroutine()` 取值，不需要 `dynamic_cast`。
- `ensure_future(shared_ptr<Coroutine<R>>)` 把已有协程登记到循环中，并返回持有它的类型化任务。
- 当前结果按值保存：函数返回 `const T&` 或 `T&&` 时，`R` 推导为 `T`；只能移动的返回值也可以保存。
- `sleep()` 记录唤醒时间，然后调用当前任务的 `yield()`。
- `run()` 每轮恢复已到时间的任务，结束或已取消的任务立即移除。循环只调用成员 `resume()`，不会消费结果。
- 所有任务都在等待时，线程休眠到最近的唤醒时间；全部结束后 `run()` 返回。
- `sleep(0ms)` 也会让出一次执行权。循环不能抢占任务，长时间计算需要主动让出。
- 任务移除后，调用方的任务句柄仍持有协程和结果，`coroutine()->result()` 取值后清空；没有其他持有者时协程才释放。
- 一个任务抛异常时，`run()` 先取消其余任务，再回抛原异常；即使调用方持有句柄，也会清理暂停任务的栈。

这里的循环运行到所有任务结束，不收集每一次暂停值。需要最终结果时，在任务结束后调用类型化句柄的 `result()`。
句柄与循环都属于当前线程，循环负责恢复执行，调用方通过句柄取结果或取消任务。

`loop_example.cpp` 展示 A 每隔 200ms 工作一次，B 每隔 100ms 工作一次。
输出带时间戳，能看到 A 休眠期间 B 继续执行，整个例子约 400ms 完成。
时间可能因线程调度而稍有延迟。A 返回工作步数 `int`，B 返回完成消息 `string`，最后通过各自的句柄输出结果。

```powershell
cmake --build build/cl --target stack_demo_3_loop
.\build\cl\stack\demo\3\Debug\stack_demo_3_loop.exe
```

Clang 构建目录对应的命令：

```powershell
cmake --build build/clang --target stack_demo_3_loop
.\build\clang\stack\demo\3\stack_demo_3_loop.exe
```

所有测试放在 `main.cpp`，每项单独写成函数，不使用测试框架。
`test_coroutine()` 运行协程本身的测试，`test_coroutine_loop()` 运行循环测试，检查：

- 任务延迟到 `run()` 执行，空循环和多次运行。
- `ensure_future()` 登记已有协程，类型化和基类访问器指向同一个对象，手动恢复一次后仍能交由循环完成。
- `spawn()` 的构造参数按值保存，`std::ref` 保留引用。
- `int`、`string`、`unique_ptr<int>`、`void` 任务交替执行，循环不消费结果，完成任务正常释放。
- 引用返回按值保存，运行前已取消的任务被跳过。
- `sleep(0ms)` 后任务交替执行的顺序。
- 普通函数里的 `sleep()` 不阻塞其他任务，且不会提前唤醒。
- 任务运行期间添加新任务。
- 拒绝任务外 `sleep()` 和递归 `run()`。
- 任务抛异常后，即使保留类型化句柄，暂停任务的局部对象仍析构、未启动任务清理、循环再次运行。

```powershell
cmake --build build/cl --target stack_demo_3
.\build\cl\stack\demo\3\Debug\stack_demo_3.exe
```

Clang 对应目标也是 `stack_demo_3`，可执行文件位于
`build/clang/stack/demo/3/stack_demo_3.exe`。

这是单线程的协作式调度：任务只操作所属线程的循环。等待 I/O 的功能暂时未加入。
