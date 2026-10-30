# 协程循环与任务

包含 `co_loop.hpp`，通过当前线程的单例创建任务：

```cpp
using namespace std::chrono_literals;
using namespace std::string_literals;

auto& loop = yq::CoLoop::instance();
auto parent = loop.spawn([] {
    auto child = yq::CoLoop::instance().spawn([] {
        yq::co::sleep(20ms);
        return 42;
    });

    int value = child->await(); // 暂停 parent，让循环执行其他任务。
    return "answer: "s + std::to_string(value);
});
loop.spawn([] {
    // parent 和 child 等待时，执行一些其他工作。
    yq::co::sleep(0ms);
});

loop.run();
std::cout << parent->result(); // answer: 42
```

- `spawn(f, args...)` 保存函数和参数，不立即执行；返回 `shared_ptr<CoTask<R>>`。引用返回按值保存，参数可以用 `std::ref`。
- `CoTask<R>` 内部持有 `Coroutine<R>`。循环通过公共基类调度，取结果时不需要类型转换。
- `result()` 只允许任务结束后调用，按值取出最终结果并清空。支持 `void` 和只能移动的值。
- `await()` 在循环任务内等待未完成的任务。已经结束的任务也可以在根上下文直接 `await()`；尚未结束时，根上下文应该先 `run()`。
- `await()` 同样消费结果；一个结果只能由一个调用者取走。自我等待和循环等待会抛出 `CoroutineException`。
- `cancel()` 取消尚未开始或暂停中的任务，展开暂停任务的局部对象；重复取消已经结束的任务直接返回。运行中或正在取消的任务不能取消。取消任务没有可取的结果。
- `co::sleep(delay)` 可以在普通函数中调用，不需要捕获协程。`sleep(0ms)` 让出一次执行权；全部可运行任务都在睡眠时，循环线程才睡眠。
- 直接 `co::yield()` 或 `co::yield(value)` 也会让出执行权，但循环不收集每次暂停值，只保留任务最终结果。
- `run()` 在根上下文调用，执行到任务列表为空；同一个循环可以重复运行。任务内可以继续 `spawn()`。
- 一个任务抛异常时，`run()` 取消剩余任务并回抛异常；失败任务第一次 `result()` 仍会抛出原异常，该次调用也会消费结果。等待者也会被取消，不能依靠 `await()` 捕获该任务异常。

所有操作都在所属线程上执行。容器使用 `list`，支持任务执行期间追加新任务。手动恢复的内层协程可以使用全局 `yield`，但不能使用外层循环任务的 `sleep` 或等待未完成任务。

`main.cpp` 中每项测试单独写成函数，分为 `test_coroutine()` 和 `test_coroutine_loop()` 两组，输出执行顺序，不使用测试框架。

```powershell
cmake --build build/clang --target stack_demo_4
.\build\clang\stack\demo\4\stack_demo_4.exe
```

MSVC 使用 `build/cl`，可执行文件位于 `build/cl/stack/demo/4/Debug/stack_demo_4.exe`。
