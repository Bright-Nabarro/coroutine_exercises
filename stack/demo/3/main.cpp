#include "yq_coroutine.hpp"
#include "co_loop.hpp"
#include <chrono>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <print>
#include <vector>

namespace {

void check(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

void check_order(const std::vector<std::string>& actual,
                 std::initializer_list<const char*> expected) {
    std::cout << "  order: ";
    for (std::size_t i = 0; i < actual.size(); ++i)
        std::cout << (i == 0 ? "" : " -> ") << actual[i];
    std::cout << '\n';

    check(actual.size() == expected.size(), "unexpected number of steps");
    std::size_t i = 0;
    for (const auto* step : expected) {
        check(actual[i] == step, "unexpected order at step " + std::to_string(i));
        ++i;
    }
}

struct TaskError : std::runtime_error {
    explicit TaskError(int number) : std::runtime_error("task failed"), code(number) {}
    int code;
};

struct TaskLocal {
    bool& destroyed;
    ~TaskLocal() { destroyed = true; }
};

void test_construction() {
    bool called = false;
    {
        yq::Coroutine<void> never_started([&] { called = true; });
        check(!called, "construction executed the task");
        check(!never_started.is_finished(), "new coroutine is already finished");
    }
    check(!called, "destruction executed an unstarted task");

    yq::Coroutine<void> co([&] { called = true; });
    co.resume();
    check(called && co.is_finished(), "task did not finish");
    check(yq::CoroutineCore::co_cur_handle == &yq::CoroutineCore::co_root,
          "current coroutine did not return to root");
}

void test_void_yield_across_function() {
    std::vector<std::string> order;
    auto ordinary_function = [&order] {
        order.push_back("function.before-yield");
        yq::co::yield();
        order.push_back("function.after-yield");
    };
    yq::Coroutine<void> co([&order, &ordinary_function] {
        order.push_back("task.begin");
        ordinary_function();
        order.push_back("task.end");
    });

    order.push_back("root.resume-1");
    yq::co::resume(co);
    order.push_back("root.after-1");
    check(!co.is_finished(), "yield was treated as completion");
    order.push_back("root.resume-2");
    co.resume();
    order.push_back("root.after-2");
    check(co.is_finished(), "second resume did not finish");
    check_order(order, {"root.resume-1", "task.begin", "function.before-yield",
                        "root.after-1", "root.resume-2", "function.after-yield",
                        "task.end", "root.after-2"});
}

void test_pause_and_final_values() {
    std::vector<std::string> order;
    yq::Coroutine<int> co([&order, &co] {
        order.push_back("task.yield-10");
        yq::co::yield(co, 10);
        order.push_back("task.yield-20");
        co.yield(20);
        order.push_back("task.return-30");
        return 30;
    });

    co.resume();
    check(co.result() == 10, "first pause value is wrong");
    order.push_back("root.got-10");
    check(!co.is_finished(), "first yield finished the task");
    co.resume();
    check(co.result() == 20, "second pause value is wrong");
    order.push_back("root.got-20");
    check(!co.is_finished(), "second yield finished the task");
    check(yq::co::resume(co) == 30, "final value is wrong");
    order.push_back("root.got-30");
    check(co.is_finished(), "final return did not finish the task");
    check_order(order, {"task.yield-10", "root.got-10", "task.yield-20",
                        "root.got-20", "task.return-30", "root.got-30"});
}

void test_nested_finished_inner() {
    std::vector<std::string> order;
    yq::Coroutine<int> outer([&order, &outer] {
        order.push_back("outer.begin");
        yq::Coroutine<int> inner([&order, &inner] {
            order.push_back("inner.yield-10");
            inner.yield(10);
            order.push_back("inner.return-20");
            return 20;
        });

        inner.resume();
        check(inner.result() == 10, "inner pause value is wrong");
        order.push_back("outer.got-10");
        check(!inner.is_finished(), "inner finished at its first yield");
        inner.resume();
        check(inner.result() == 20, "inner final value is wrong");
        order.push_back("outer.got-20");
        check(inner.is_finished(), "inner did not finish");

        // inner 已结束，但对象还在本作用域中；这是之前的嵌套问题。
        order.push_back("outer.yield-100");
        outer.yield(100);
        order.push_back("outer.return-200");
        return 200;
    });

    outer.resume();
    check(outer.result() == 100, "outer did not yield back to root");
    order.push_back("root.got-100");
    check(!outer.is_finished(), "outer continued past its yield");
    outer.resume();
    check(outer.result() == 200, "outer final value is wrong");
    order.push_back("root.got-200");
    check(outer.is_finished(), "outer did not finish");
    check_order(order, {"outer.begin", "inner.yield-10", "outer.got-10",
                        "inner.return-20", "outer.got-20", "outer.yield-100",
                        "root.got-100", "outer.return-200", "root.got-200"});
}

void test_caller_changes() {
    std::vector<std::string> order;
    // inner 先创建，也可以由之后创建的 outer 恢复。
    yq::Coroutine<void> inner([&order] {
        order.push_back("inner.first");
        yq::co::yield();
        order.push_back("inner.end");
    });
    yq::Coroutine<void> outer([&order, &inner] {
        auto* outer_core = yq::CoroutineCore::co_cur_handle;
        order.push_back("outer.before-inner");
        inner.resume();
        check(yq::CoroutineCore::co_cur_handle == outer_core,
              "inner yield did not restore the outer coroutine");
        order.push_back("outer.after-inner");
        yq::co::yield();
        check(yq::CoroutineCore::co_cur_handle == outer_core,
              "outer resume did not restore the current coroutine");
        order.push_back("outer.end");
    });

    outer.resume();
    order.push_back("root.after-outer-1");
    check(!outer.is_finished() && !inner.is_finished(), "unexpected completion");
    // 第二次直接从根部恢复 inner，不能继续返回到旧的 outer。
    inner.resume();
    order.push_back("root.after-inner-2");
    check(inner.is_finished(), "inner did not return to its new caller");
    outer.resume();
    order.push_back("root.after-outer-2");
    check(outer.is_finished(), "outer did not finish");
    check_order(order, {"outer.before-inner", "inner.first", "outer.after-inner",
                        "root.after-outer-1", "inner.end", "root.after-inner-2",
                        "outer.end", "root.after-outer-2"});
}

void test_constructor_arguments() {
    int number = 3;
    yq::Coroutine<int> by_value([](int n) { return n * 2; }, number);
    yq::Coroutine<int> by_reference([](int& n) { return ++n; }, std::ref(number));
    number = 10;
    by_value.resume();
    check(by_value.result() == 6, "value argument was not saved at construction");
    by_reference.resume();
    check(by_reference.result() == 11 && number == 11, "std::ref did not preserve the reference");

    struct Counter {
        int value = 0;
        int add(int n) { return value += n; }
    } counter;
    yq::Coroutine<int> member(std::size_t{256 * 1024}, &Counter::add, &counter, 7);
    member.resume();
    check(member.result() == 7 && counter.value == 7, "member function invocation failed");
}

void test_move_only_results() {
    yq::Coroutine<std::unique_ptr<int>> co([&co] {
        co.yield(std::make_unique<int>(7));
        return std::make_unique<int>(8);
    });

    auto unused_value = std::make_unique<int>(99);
    bool caught = false;
    try {
        co.yield(std::move(unused_value)); // 根部不能调用 co 的 yield。
    } catch (const yq::CoroutineException&) {
        caught = true;
    }
    check(caught, "yield from root was not rejected");
    check(unused_value && *unused_value == 99, "invalid yield consumed its argument");

    co.resume();
    auto paused_value = co.result();
    check(paused_value && *paused_value == 7 && !co.is_finished(), "move-only pause value is wrong");
    auto final_value = yq::co::resume(co);
    check(final_value && *final_value == 8 && co.is_finished(), "move-only final value is wrong");
}

void test_base_resume_and_result() {
    std::vector<std::string> order;
    bool running_result_rejected = false;
    yq::Coroutine<int> co([&order, &running_result_rejected, &co] {
        order.push_back("task.begin");
        try { static_cast<void>(co.result()); }
        catch (const yq::CoroutineException&) { running_result_rejected = true; }
        co.yield(); // int 协程也允许无值暂停。
        order.push_back("task.after-empty-yield");
        co.yield(10);
        order.push_back("task.after-value-yield");
        return 20;
    });
    yq::CoroutineBase& base = co;

    bool caught = false;
    try { static_cast<void>(co.result()); }
    catch (const yq::CoroutineException&) { caught = true; }
    check(caught, "result before first resume was accepted");
    check(base.status() == yq::CoroutineStatus::Ready, "base reported an incorrect initial status");

    order.push_back("root.base-resume-1");
    yq::co::resume(base); // 基类重载只恢复，不能尝试读取空结果槽。
    order.push_back("root.after-1");
    check(running_result_rejected, "result while running was accepted");
    check(base.status() == yq::CoroutineStatus::Suspended && !base.is_finished(),
          "base resume did not stop at the unvalued yield");
    caught = false;
    try { static_cast<void>(co.result()); }
    catch (const yq::CoroutineException&) { caught = true; }
    check(caught, "unvalued yield produced a result");

    order.push_back("root.typed-resume-2");
    check(yq::co::resume(co) == 10, "typed resume helper did not return the pause value");
    order.push_back("root.got-10");
    caught = false;
    try { static_cast<void>(co.result()); }
    catch (const yq::CoroutineException&) { caught = true; }
    check(caught, "typed resume helper did not consume the result");

    order.push_back("root.base-resume-3");
    base.resume();
    order.push_back("root.after-3");
    check(base.is_finished() && base.status() == yq::CoroutineStatus::Finished,
          "base did not report completion");
    check(co.result() == 20, "base resume consumed or lost the final result");
    order.push_back("root.got-20");
    caught = false;
    try { static_cast<void>(co.result()); }
    catch (const yq::CoroutineException&) { caught = true; }
    check(caught, "final result was available more than once");
    check_order(order, {"root.base-resume-1", "task.begin", "root.after-1",
                        "root.typed-resume-2", "task.after-empty-yield", "root.got-10",
                        "root.base-resume-3", "task.after-value-yield", "root.after-3", "root.got-20"});
}

void test_typed_resume_without_value() {
    std::vector<std::string> order;
    yq::Coroutine<int> co([&order] {
        order.push_back("task.empty-yield");
        yq::co::yield();
        order.push_back("task.return-42");
        return 42;
    });

    bool caught = false;
    try { static_cast<void>(yq::co::resume(co)); }
    catch (const yq::CoroutineException&) {
        caught = true;
        order.push_back("root.no-result");
    }
    check(caught && co.status() == yq::CoroutineStatus::Suspended,
          "typed resume helper accepted an empty result or changed the paused state");
    check(yq::co::resume(co) == 42 && co.is_finished(),
          "task could not finish after an empty result was rejected");
    order.push_back("root.got-42");
    check_order(order, {"task.empty-yield", "root.no-result", "task.return-42", "root.got-42"});
}

void test_base_destruction_cleanup() {
    bool local_destroyed = false;
    std::unique_ptr<yq::CoroutineBase> co;
    co = std::make_unique<yq::Coroutine<int>>([&local_destroyed, &co] {
        TaskLocal local{local_destroyed};
        co->yield();
        return 42;
    });
    co->resume();
    check(!local_destroyed, "base resume destroyed the paused task local");
    co.reset(); // 必须通过虚析构进入 Coroutine<int> 的取消和清理。
    check(local_destroyed, "base destruction skipped the paused task's local destructor");
    check(yq::CoroutineCore::co_cur_handle == &yq::CoroutineCore::co_root,
          "base destruction did not restore root");
}

void test_invalid_operations() {
    std::vector<std::string> order;
    bool root_yield_rejected = false;
    try { yq::co::yield(); }
    catch (const yq::CoroutineException&) { root_yield_rejected = true; }
    check(root_yield_rejected, "global yield from root was not rejected");
    check(yq::CoroutineCore::co_cur_handle == &yq::CoroutineCore::co_root,
          "rejected root yield changed the current coroutine");

    yq::Coroutine<void> co([&order, &co] {
        order.push_back("task.begin");
        bool caught = false;
        try {
            co.resume();
        } catch (const yq::CoroutineException&) {
            caught = true;
            order.push_back("task.self-resume-rejected");
        }
        check(caught, "self resume was not rejected");
        co.yield();
        order.push_back("task.end");
    });

    bool caught = false;
    try { co.yield(); }
    catch (const yq::CoroutineException&) { caught = true; }
    check(caught && order.empty(), "yield before first resume was not rejected");

    co.resume();
    order.push_back("root.after-yield");
    caught = false;
    try { co.yield(); }
    catch (const yq::CoroutineException&) { caught = true; }
    check(caught, "yield on a paused coroutine was not rejected");

    co.resume();
    order.push_back("root.after-end");
    caught = false;
    try { co.resume(); }
    catch (const yq::CoroutineException&) { caught = true; }
    check(caught, "resume after completion was not rejected");
    check_order(order, {"task.begin", "task.self-resume-rejected", "root.after-yield",
                        "task.end", "root.after-end"});
}

void test_yield_other_coroutine() {
    bool other_ran = false;
    yq::Coroutine<void> other([&] { other_ran = true; });
    yq::Coroutine<void> runner([&] {
        bool caught = false;
        try { other.yield(); }
        catch (const yq::CoroutineException&) { caught = true; }
        check(caught, "yield on another coroutine was not rejected");
    });
    runner.resume();
    check(runner.is_finished() && !other_ran, "invalid yield changed execution");
    other.resume();
    check(other_ran && other.is_finished(), "other coroutine could not run afterward");
}

void test_task_exceptions() {
    yq::Coroutine<int> immediate([]() -> int { throw TaskError{1}; });
    bool caught = false;
    try { immediate.resume(); }
    catch (const TaskError& error) { caught = error.code == 1; }
    check(caught, "exception before first yield lost its original type");

    std::vector<std::string> order;
    bool local_destroyed = false;
    yq::Coroutine<int> delayed([&order, &local_destroyed, &delayed]() -> int {
        TaskLocal local{local_destroyed};
        order.push_back("task.yield-10");
        delayed.yield(10);
        order.push_back("task.throw");
        throw TaskError{2};
    });
    delayed.resume();
    check(delayed.result() == 10, "pause before exception failed");
    order.push_back("root.got-10");
    check(!local_destroyed, "task local was destroyed at yield");
    caught = false;
    try { delayed.resume(); }
    catch (const TaskError& error) {
        caught = error.code == 2;
        order.push_back("root.caught-task-error");
    }
    check(caught && local_destroyed, "exception did not unwind task locals");
    check(yq::CoroutineCore::co_cur_handle == &yq::CoroutineCore::co_root,
          "exception did not restore root");
    yq::Coroutine<int> next([] { return 42; });
    next.resume();
    check(next.result() == 42, "could not run another task after the exception");
    check_order(order, {"task.yield-10", "root.got-10", "task.throw", "root.caught-task-error"});
}

void test_completed_task_cleanup() {
    bool local_destroyed = false;
    yq::Coroutine<void> co([&local_destroyed, &co] {
        TaskLocal local{local_destroyed};
        co.yield();
    });
    co.resume();
    check(!local_destroyed, "yield destroyed the task local");
    co.resume();
    check(co.is_finished() && local_destroyed, "completion did not destroy the task local");
}

// 销毁暂停协程时，cancel() 应展开任务栈并析构局部对象。
void test_paused_task_cleanup() {
    bool local_destroyed = false;
    {
        yq::Coroutine<void> co([&local_destroyed] {
            TaskLocal local{local_destroyed};
            yq::co::yield();
        });
        co.resume();
        check(!local_destroyed, "task local did not survive the pause");
    }
    check(local_destroyed, "paused task local destructor was skipped (see ISSUE.md #1)");
}

void test_current_yield_during_cancel() {
    std::vector<std::string> order;
    bool local_destroyed = false;
    yq::Coroutine<void> co([&order, &local_destroyed] {
        TaskLocal local{local_destroyed};
        order.push_back("task.pause");
        try {
            yq::co::yield();
        } catch (const yq::Cancelled&) {
            order.push_back("task.caught-cancel");
        }
        // 吞掉取消异常后再次暂停，也必须抛出，不能切回调用者。
        try {
            yq::co::yield();
            order.push_back("task.unexpected-resume");
        } catch (const yq::Cancelled&) {
            order.push_back("task.second-yield-rejected");
        }
        order.push_back("task.end");
    });
    co.resume();
    order.push_back("root.cancel");
    co.cancel();
    order.push_back("root.after-cancel");
    check(co.is_finished() && local_destroyed, "global yield cancellation did not clean up the task");
    check(yq::CoroutineCore::co_cur_handle == &yq::CoroutineCore::co_root,
          "global yield cancellation did not restore root");
    check_order(order, {"task.pause", "root.cancel", "task.caught-cancel",
                        "task.second-yield-rejected", "task.end", "root.after-cancel"});
}

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

void test_interface() {
    auto& loop = yq::CoLoop::instance();
    std::vector<std::string> order;
    std::shared_ptr<yq::Coroutine<int>> co;
    co = std::make_shared<yq::Coroutine<int>>([&order, &co] {
        order.push_back("task.yield-1");
        co->yield(1);
        order.push_back("task.yield-2");
        co->yield(2);
        order.push_back("task.return-3");
        return 3;
    });
    auto task = loop.ensure_future(co);
    check(order.empty(), "ensure_future executed the coroutine immediately");
    check(task->coroutine() == co && task->base_coroutine() == co,
          "task getters did not refer to the original coroutine");
    task->coroutine()->resume();
    check(task->coroutine()->result() == 1, "registered coroutine lost its first pause value");
    order.push_back("root.got-1");
    loop.run();
    check(task->coroutine()->is_finished(), "loop did not finish the registered coroutine");
    check(task->coroutine()->result() == 3, "registered coroutine lost its final value");
    order.push_back("root.got-3");
    check_order(order, {"task.yield-1", "root.got-1", "task.yield-2", "task.return-3", "root.got-3"});
}

void test_loop_spawn_and_reuse() {
    auto& loop = yq::CoLoop::instance();
    int calls = 0;
    loop.run(); // 空循环应当直接返回。

    loop.spawn([&calls] { ++calls; });
    check(calls == 0, "spawn executed the task immediately");
    loop.run();
    check(calls == 1, "run did not execute the task");
    loop.run();
    check(calls == 1, "finished task ran again");

    loop.spawn([&calls] { ++calls; });
    loop.run();
    check(calls == 2, "loop could not run a new batch");
}

void test_loop_spawn_arguments() {
    auto& loop = yq::CoLoop::instance();
    int number = 3;
    auto by_value = loop.spawn([](int n) { return n; }, number);
    auto by_reference = loop.spawn([](int& n) { return ++n; }, std::ref(number));
    number = 10;
    loop.run();
    check(by_value->coroutine()->result() == 3, "spawn did not save its value argument");
    check(by_reference->coroutine()->result() == 11 && number == 11, "spawn did not preserve std::ref");
}

void test_loop_typed_results() {
    auto& loop = yq::CoLoop::instance();
    std::vector<std::string> order;
    auto number = loop.spawn([&loop, &order](int n) {
        order.push_back("number.begin");
        loop.sleep(0ms);
        order.push_back("number.end");
        return n + 1;
    }, 41);
    auto text = loop.spawn([&order] {
        order.push_back("text.work");
        return std::string{"done"};
    });
    auto owned = loop.spawn([&loop, &order] {
        order.push_back("owned.begin");
        loop.sleep(0ms);
        order.push_back("owned.end");
        return std::make_unique<int>(7);
    });
    auto no_result = loop.spawn([&order] { order.push_back("void.work"); });
    std::weak_ptr<yq::CoTask<int>> observed = number;
    std::weak_ptr<yq::Coroutine<int>> observed_coroutine = number->coroutine();

    loop.run();
    check_order(order, {"number.begin", "text.work", "owned.begin", "void.work",
                        "number.end", "owned.end"});
    check(number->coroutine()->is_finished() && text->coroutine()->is_finished()
          && owned->coroutine()->is_finished() && no_result->coroutine()->is_finished(),
          "loop left a typed task unfinished");
    // 直接使用各自的类型化句柄取值，不做 dynamic_cast。
    check(number->coroutine()->result() == 42, "loop consumed or lost the int result");
    check(text->coroutine()->result() == "done", "loop consumed or lost the string result");
    auto value = owned->coroutine()->result();
    check(value && *value == 7, "loop could not return a move-only result");
    no_result->coroutine()->result();

    bool caught = false;
    try { static_cast<void>(number->coroutine()->result()); }
    catch (const yq::CoroutineException&) { caught = true; }
    check(caught, "loop result could be consumed more than once");
    number.reset();
    check(observed.expired(), "loop retained a finished task after the caller released it");
    check(observed_coroutine.expired(), "finished task did not release its coroutine");
}

void test_loop_reference_return_as_value() {
    auto& loop = yq::CoLoop::instance();
    std::string source = "saved";
    // 当前协程只保存值，spawn 应把 const string& 推导成 string。
    std::shared_ptr<yq::CoTask<std::string>> co = loop.spawn([&source]() -> const std::string& {
        return source;
    });
    loop.run();
    source = "changed";
    check(co->coroutine()->result() == "saved", "loop retained a reference instead of saving a value");
}

void test_loop_cancel_before_run() {
    auto& loop = yq::CoLoop::instance();
    bool called = false;
    auto co = loop.spawn([&called] {
        called = true;
        return 42;
    });
    co->coroutine()->cancel();
    loop.run();
    check(!called && co->coroutine()->is_finished(), "loop resumed a task cancelled before run");
}

void test_loop_zero_sleep_order() {
    auto& loop = yq::CoLoop::instance();
    std::vector<std::string> order;
    loop.spawn([&loop, &order] {
        order.push_back("A.1");
        loop.sleep(0ms);
        order.push_back("A.2");
        loop.sleep(0ms);
        order.push_back("A.3");
    });
    loop.spawn([&loop, &order] {
        order.push_back("B.1");
        loop.sleep(0ms);
        order.push_back("B.2");
    });
    loop.run();
    check_order(order, {"A.1", "B.1", "A.2", "B.2", "A.3"});
}



// 普通函数不需要拿到 Coroutine 对象，也可以暂停当前任务。
void sleep_in_loop_function(std::chrono::milliseconds delay) {
    yq::CoLoop::instance().sleep(delay);
}

void test_loop_sleep_and_other_work() {
    auto& loop = yq::CoLoop::instance();
    std::vector<std::string> order;
    Clock::duration elapsed{};
    loop.spawn([&order, &elapsed] {
        order.push_back("slow.begin");
        const auto before = Clock::now();
        sleep_in_loop_function(20ms);
        elapsed = Clock::now() - before;
        order.push_back("slow.end");
    });
    loop.spawn([&order] {
        order.push_back("fast.begin");
        order.push_back("fast.end");
    });
    loop.run();
    check_order(order, {"slow.begin", "fast.begin", "fast.end", "slow.end"});
    // 只检查没有提前唤醒，允许系统调度带来的延迟。
    check(elapsed >= 20ms, "sleep woke the task before its deadline");
}

void test_loop_spawn_during_run() {
    auto& loop = yq::CoLoop::instance();
    std::vector<std::string> order;
    std::shared_ptr<yq::CoTask<int>> child;
    auto parent = loop.spawn([&loop, &order, &child] {
        order.push_back("parent.begin");
        child = loop.spawn([&order] {
            order.push_back("child.work");
            return 42;
        });
        order.push_back("parent.before-sleep");
        loop.sleep(0ms);
        order.push_back("parent.end");
        return std::string{"parent done"};
    });
    loop.run();
    check_order(order, {"parent.begin", "parent.before-sleep", "child.work", "parent.end"});
    check(parent->coroutine()->result() == "parent done" && child && child->coroutine()->result() == 42,
          "tasks spawned during run lost their typed results");
}

void test_loop_invalid_operations() {
    auto& loop = yq::CoLoop::instance();
    bool sleep_rejected = false;
    try {
        loop.sleep(0ms);
    } catch (const yq::CoroutineException&) {
        sleep_rejected = true;
    }
    check(sleep_rejected, "sleep outside a loop task was accepted");

    bool run_rejected = false;
    bool continued = false;
    loop.spawn([&loop, &run_rejected, &continued] {
        try {
            loop.run();
        } catch (const yq::CoroutineException&) {
            run_rejected = true;
        }
        loop.sleep(0ms);
        continued = true;
    });
    loop.run();
    check(run_rejected, "recursive run was accepted");
    check(continued, "rejected recursive run broke the current task");
}

void test_loop_exception_cleanup_and_reuse() {
    auto& loop = yq::CoLoop::instance();
    bool local_destroyed = false;
    bool after_sleep = false;
    bool queued_task_ran = false;
    auto paused = loop.spawn([&loop, &local_destroyed, &after_sleep] {
        TaskLocal local{local_destroyed};
        loop.sleep(1h);
        after_sleep = true;
        return 7;
    });
    auto failed = loop.spawn([&local_destroyed]() -> int {
        check(!local_destroyed, "paused task local was destroyed too early");
        throw TaskError{3};
    });
    auto queued = loop.spawn([&queued_task_ran] {
        queued_task_ran = true;
        return 9;
    });

    bool exception_received = false;
    try {
        loop.run();
    } catch (const TaskError& error) {
        exception_received = error.code == 3;
    }
    check(exception_received, "task exception did not reach run's caller");
    check(local_destroyed, "cleanup skipped the paused task's local destructor");
    check(!after_sleep, "cancelled task continued past sleep");
    check(!queued_task_ran, "queued task ran after another task failed");
    check(paused->coroutine()->is_finished() && queued->coroutine()->is_finished()
          && failed->coroutine()->is_finished(),
          "cleanup left externally owned tasks resumable");
    bool stored_exception_received = false;
    try { static_cast<void>(failed->coroutine()->result()); }
    catch (const TaskError& error) { stored_exception_received = error.code == 3; }
    check(stored_exception_received, "failed task handle lost its original exception");
    check(yq::CoroutineCore::co_cur_handle == &yq::CoroutineCore::co_root,
          "cleanup did not restore the root coroutine");

    bool new_task_ran = false;
    loop.spawn([&new_task_ran] { new_task_ran = true; });
    loop.run();
    check(new_task_ran && !queued_task_ran, "loop did not recover after task failure");
}

void test_coroutine(auto& test) {
    std::cout << "\n=== Coroutine tests ===\n";
    test("construction does not execute the task", test_construction);
    test("void yield crosses an ordinary function", test_void_yield_across_function);
    test("pause values and final value", test_pause_and_final_values);
    test("outer yields while finished inner is still alive", test_nested_finished_inner);
    test("caller changes from outer to root", test_caller_changes);
    test("constructor arguments and member function", test_constructor_arguments);
    test("move-only results and invalid value yield", test_move_only_results);
    test("base resume preserves typed results and result is consumed once", test_base_resume_and_result);
    test("typed resume helper rejects an unvalued pause", test_typed_resume_without_value);
    test("base destruction cleans up a paused typed coroutine", test_base_destruction_cleanup);
    test("invalid void yield, self resume, and finished resume", test_invalid_operations);
    test("cannot yield another coroutine", test_yield_other_coroutine);
    test("task exception keeps its type and restores root", test_task_exceptions);
    test("completed task destroys its local objects", test_completed_task_cleanup);
    test("destroying a paused task must clean up locals", test_paused_task_cleanup);
    test("current yield rejects a new pause during cancellation", test_current_yield_during_cancel);
}

void test_coroutine_loop(auto& test) {
    std::cout << "\n=== Coroutine loop tests ===\n";
    test("ensure_future registers an existing coroutine and preserves its result", test_interface);
    test("loop spawn and reuse", test_loop_spawn_and_reuse);
    test("loop spawn forwards constructor arguments", test_loop_spawn_arguments);
    test("loop preserves typed results and releases finished tasks", test_loop_typed_results);
    test("loop saves a reference return as a value", test_loop_reference_return_as_value);
    test("loop skips a task cancelled before run", test_loop_cancel_before_run);
    test("loop zero sleep order", test_loop_zero_sleep_order);
    test("loop sleep and other work", test_loop_sleep_and_other_work);
    test("loop spawn during run", test_loop_spawn_during_run);
    test("loop invalid operations", test_loop_invalid_operations);
    test("loop exception cleanup and reuse", test_loop_exception_cleanup_and_reuse);
}

} // namespace

int main() {
    int passed = 0;
    int failed = 0;

    // 只有简单的报告器；每项里的 resume/yield 和检查都直接写出来。
    auto test = [&](const char* name, void (*body)()) {
        std::cout << "\n[TEST] " << name << std::endl;
        try {
            body();
            ++passed;
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& error) {
            ++failed;
            std::cout << "[FAIL] " << name << ": " << error.what() << '\n';
        } catch (...) {
            ++failed;
            std::cout << "[FAIL] " << name << ": unexpected exception type\n";
        }
    };

    test_coroutine(test);
    test_coroutine_loop(test);

    std::cout << "\nResult: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
