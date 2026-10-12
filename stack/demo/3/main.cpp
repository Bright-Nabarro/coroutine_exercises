#include "yq_coroutine.hpp"
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
    auto ordinary_function = [&](yq::Coroutine<void>& co) {
        order.push_back("function.before-yield");
        yq::co::yield(co);
        order.push_back("function.after-yield");
    };
    yq::Coroutine<void> co([&order, &ordinary_function, &co] {
        order.push_back("task.begin");
        ordinary_function(co);
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

    check(co.resume() == 10, "first pause value is wrong");
    order.push_back("root.got-10");
    check(!co.is_finished(), "first yield finished the task");
    check(co.resume() == 20, "second pause value is wrong");
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

        check(inner.resume() == 10, "inner pause value is wrong");
        order.push_back("outer.got-10");
        check(!inner.is_finished(), "inner finished at its first yield");
        check(inner.resume() == 20, "inner final value is wrong");
        order.push_back("outer.got-20");
        check(inner.is_finished(), "inner did not finish");

        // inner 已结束，但对象还在本作用域中；这是之前的嵌套问题。
        order.push_back("outer.yield-100");
        outer.yield(100);
        order.push_back("outer.return-200");
        return 200;
    });

    check(outer.resume() == 100, "outer did not yield back to root");
    order.push_back("root.got-100");
    check(!outer.is_finished(), "outer continued past its yield");
    check(outer.resume() == 200, "outer final value is wrong");
    order.push_back("root.got-200");
    check(outer.is_finished(), "outer did not finish");
    check_order(order, {"outer.begin", "inner.yield-10", "outer.got-10",
                        "inner.return-20", "outer.got-20", "outer.yield-100",
                        "root.got-100", "outer.return-200", "root.got-200"});
}

void test_caller_changes() {
    std::vector<std::string> order;
    // inner 先创建，也可以由之后创建的 outer 恢复。
    yq::Coroutine<void> inner([&order, &inner] {
        order.push_back("inner.first");
        inner.yield();
        order.push_back("inner.end");
    });
    yq::Coroutine<void> outer([&order, &inner, &outer] {
        order.push_back("outer.before-inner");
        inner.resume();
        order.push_back("outer.after-inner");
        outer.yield();
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
    check(by_value.resume() == 6, "value argument was not saved at construction");
    check(by_reference.resume() == 11 && number == 11, "std::ref did not preserve the reference");

    struct Counter {
        int value = 0;
        int add(int n) { return value += n; }
    } counter;
    yq::Coroutine<int> member(std::size_t{256 * 1024}, &Counter::add, &counter, 7);
    check(member.resume() == 7 && counter.value == 7, "member function invocation failed");
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

    auto paused_value = co.resume();
    check(paused_value && *paused_value == 7 && !co.is_finished(), "move-only pause value is wrong");
    auto final_value = co.resume();
    check(final_value && *final_value == 8 && co.is_finished(), "move-only final value is wrong");
}

void test_invalid_operations() {
    std::vector<std::string> order;
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
    try { static_cast<void>(immediate.resume()); }
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
    check(delayed.resume() == 10, "pause before exception failed");
    order.push_back("root.got-10");
    check(!local_destroyed, "task local was destroyed at yield");
    caught = false;
    try { static_cast<void>(delayed.resume()); }
    catch (const TaskError& error) {
        caught = error.code == 2;
        order.push_back("root.caught-task-error");
    }
    check(caught && local_destroyed, "exception did not unwind task locals");
    check(yq::CoroutineCore::co_cur_handle == &yq::CoroutineCore::co_root,
          "exception did not restore root");
    yq::Coroutine<int> next([] { return 42; });
    check(next.resume() == 42, "could not run another task after the exception");
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

// 已知问题，故意保留失败：不泄漏真实堆资源，只观察析构是否发生。
void test_paused_task_cleanup() {
    bool local_destroyed = false;
    {
        yq::Coroutine<void> co([&local_destroyed, &co] {
            TaskLocal local{local_destroyed};
            co.yield();
        });
        co.resume();
        check(!local_destroyed, "task local did not survive the pause");
    }
    check(local_destroyed, "paused task local destructor was skipped (see ISSUE.md #1)");
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

    test("construction does not execute the task", test_construction);
    test("void yield crosses an ordinary function", test_void_yield_across_function);
    test("pause values and final value", test_pause_and_final_values);
    test("outer yields while finished inner is still alive", test_nested_finished_inner);
    test("caller changes from outer to root", test_caller_changes);
    test("constructor arguments and member function", test_constructor_arguments);
    test("move-only results and invalid value yield", test_move_only_results);
    test("invalid void yield, self resume, and finished resume", test_invalid_operations);
    test("cannot yield another coroutine", test_yield_other_coroutine);
    test("task exception keeps its type and restores root", test_task_exceptions);
    test("completed task destroys its local objects", test_completed_task_cleanup);
    test("destroying a paused task must clean up locals", test_paused_task_cleanup);

    std::cout << "\nResult: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
