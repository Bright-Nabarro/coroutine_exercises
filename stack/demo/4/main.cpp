#include "coroutine.hpp"
#include "co_loop.hpp"
#include <chrono>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
using namespace std;

namespace {

using namespace std::string_literals;

void check(bool condition, const char* message) {
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
    for (const auto* step : expected)
        check(actual[i++] == step, "unexpected execution order");
}

void check_root() {
    check(yq::CoroutineCore::co_cur_handle == &yq::CoroutineCore::co_root,
          "current coroutine did not return to root");
}

void test_void_yield_across_function() {
    std::vector<std::string> order;
    auto work = [&order] {
        order.push_back("function.begin");
        yq::co::yield(); // 普通函数不用接收或捕获协程对象。
        order.push_back("function.end");
    };
    yq::Coroutine<void> co([&order, &work] {
        order.push_back("task.begin");
        work();
        order.push_back("task.end");
    });
    check(order.empty(), "construction executed the task");
    co.resume();
    order.push_back("root.after-pause");
    check(co.status() == yq::CoroutineStatus::Suspended, "yield did not suspend");
    check_root();
    co.resume();
    co.result();
    order.push_back("root.after-finish");
    check(co.is_finished(), "second resume did not finish");
    check_root();
    check_order(order, {"task.begin", "function.begin", "root.after-pause",
                        "function.end", "task.end", "root.after-finish"});
}

void test_pause_and_final_values() {
    std::vector<std::string> order;
    yq::Coroutine<int> co([&order] {
        order.push_back("task.yield-10");
        yq::co::yield(10);
        order.push_back("task.yield-20");
        yq::co::yield(20);
        order.push_back("task.return-30");
        return 30;
    });
    yq::CoroutineBase& base = co;
    base.resume(); // 恢复执行与取结果分开。
    check(co.result() == 10 && !co.is_finished(), "first pause value is wrong");
    order.push_back("root.got-10");
    check(yq::co::resume(co) == 20 && !co.is_finished(), "second pause value is wrong");
    order.push_back("root.got-20");
    check(yq::co::resume(co) == 30 && co.is_finished(), "final value is wrong");
    order.push_back("root.got-30");
    bool rejected = false;
    try { static_cast<void>(co.result()); }
    catch (const yq::CoroutineException&) { rejected = true; }
    check(rejected, "result could be consumed twice");
    check_root();
    check_order(order, {"task.yield-10", "root.got-10", "task.yield-20",
                        "root.got-20", "task.return-30", "root.got-30"});
}

void test_type_mismatch_and_string_values() {
    std::vector<std::string> order;
    yq::Coroutine<std::string> co([&order] {
        bool rejected = false;
        try { yq::co::yield(123); } // int 不等于当前协程的 string 结果类型。
        catch (const yq::CoroutineException&) {
            rejected = true;
            order.push_back("task.type-rejected");
        }
        check(rejected, "yield accepted a different result type");
        check(yq::CoroutineCore::co_cur_handle->status == yq::CoroutineStatus::Running,
              "type mismatch suspended the task");
        yq::co::yield("working"s);
        order.push_back("task.finish");
        return "done"s;
    });
    co.resume();
    check(co.result() == "working"s && !co.is_finished(), "string pause value is wrong");
    order.push_back("root.got-working");
    co.resume();
    check(co.result() == "done"s && co.is_finished(), "string final value is wrong");
    order.push_back("root.got-done");
    check_root();
    check_order(order, {"task.type-rejected", "root.got-working", "task.finish", "root.got-done"});
}

void test_nested_coroutines() {
    std::vector<std::string> order;
    yq::Coroutine<int> outer([&order] {
        auto* outer_core = yq::CoroutineCore::co_cur_handle;
        yq::Coroutine<std::string> inner([&order] {
            order.push_back("inner.yield");
            yq::co::yield("inner pause"s);
            order.push_back("inner.finish");
            return "inner done"s;
        });
        inner.resume();
        check(inner.result() == "inner pause"s, "inner pause value is wrong");
        check(yq::CoroutineCore::co_cur_handle == outer_core, "inner pause did not restore outer");
        order.push_back("outer.after-inner-pause");
        inner.resume();
        check(inner.result() == "inner done"s && inner.is_finished(), "inner did not finish");
        check(yq::CoroutineCore::co_cur_handle == outer_core, "inner finish did not restore outer");
        order.push_back("outer.after-inner-finish");
        // inner 已结束但对象尚未析构；此时全局 yield 必须使用 outer 的 int 槽位。
        yq::co::yield(100);
        order.push_back("outer.finish");
        return 200;
    });
    outer.resume();
    check(outer.result() == 100 && !outer.is_finished(), "outer did not pause back to root");
    order.push_back("root.got-100");
    check_root();
    outer.resume();
    check(outer.result() == 200 && outer.is_finished(), "outer final value is wrong");
    order.push_back("root.got-200");
    check_root();
    check_order(order, {"inner.yield", "outer.after-inner-pause", "inner.finish",
                        "outer.after-inner-finish", "root.got-100", "outer.finish", "root.got-200"});
}

void test_move_only_results() {
    yq::Coroutine<std::unique_ptr<int>> co([] {
        yq::co::yield(std::make_unique<int>(7));
        return std::make_unique<int>(8);
    });
    co.resume();
    auto paused = co.result();
    check(paused && *paused == 7 && !co.is_finished(), "move-only pause value is wrong");
    co.resume();
    auto finished = co.result();
    check(finished && *finished == 8 && co.is_finished(), "move-only final value is wrong");
    check_root();
}

struct TaskLocal {
    bool& destroyed;
    ~TaskLocal() { destroyed = true; }
};

void test_cancellation_cleanup() {
    std::vector<std::string> order;
    bool destroyed = false;
    {
        yq::Coroutine<void> co([&order, &destroyed] {
            TaskLocal local{destroyed};
            order.push_back("task.pause");
            try { yq::co::yield(); }
            catch (const yq::Cancelled&) { order.push_back("task.caught-cancel"); }
            order.push_back("task.try-yield-again");
            yq::co::yield(); // 仍处于 Cancelling，必须抛出并展开任务栈。
            order.push_back("task.unexpected-continue");
        });
        co.resume();
        check(!destroyed, "yield destroyed a task local");
        order.push_back("root.destroy");
    }
    order.push_back("root.after-destroy");
    check(destroyed, "destruction did not unwind the paused task");
    check_root();
    check_order(order, {"task.pause", "root.destroy", "task.caught-cancel",
                        "task.try-yield-again", "root.after-destroy"});
}

void test_root_yield_and_task_exception() {
    bool rejected = false;
    try { yq::co::yield(); }
    catch (const yq::CoroutineException&) { rejected = true; }
    check(rejected, "root was allowed to yield without a value");
    rejected = false;
    try { yq::co::yield(10); }
    catch (const yq::CoroutineException&) { rejected = true; }
    check(rejected, "root was allowed to yield a value");
    check_root();

    struct TaskError : std::runtime_error {
        TaskError() : std::runtime_error("task failed") {}
    };
    bool destroyed = false;
    yq::Coroutine<int> co([&destroyed]() -> int {
        TaskLocal local{destroyed};
        yq::co::yield(1);
        throw TaskError{};
    });
    co.resume();
    check(co.result() == 1 && !destroyed, "pause before task exception failed");
    bool caught = false;
    try { co.resume(); }
    catch (const TaskError&) { caught = true; }
    check(caught && destroyed && co.is_finished(), "task exception was lost or did not unwind locals");
    check_root();
    yq::Coroutine<int> next([] { return 42; });
    next.resume();
    check(next.result() == 42, "could not run a new coroutine after a task exception");
}

using namespace std::chrono_literals;

void test_loop_typed_results_and_order() {
    auto& loop = yq::CoLoop::instance();
    std::vector<std::string> order;
    auto number = loop.spawn([&order](int n) {
        order.push_back("number.begin");
        yq::co::yield(10);
        order.push_back("number.end");
        return n + 1;
    }, 41);
    auto text = loop.spawn([&order] {
        order.push_back("text.work");
        return "done"s;
    });
    auto owned = loop.spawn([&order] {
        order.push_back("owned.begin");
        yq::co::yield(std::make_unique<int>(7));
        order.push_back("owned.end");
        return std::make_unique<int>(8);
    });
    auto nothing = loop.spawn([&order] {
        order.push_back("void.begin");
        yq::co::sleep(0ms);
        order.push_back("void.end");
    });
    check(order.empty(), "spawn executed the task immediately");
    bool rejected = false;
    try { static_cast<void>(number->result()); }
    catch (const yq::CoroutineException&) { rejected = true; }
    check(rejected, "task result was available before completion");
    std::weak_ptr<yq::CoTask<int>> observed = number;
    loop.run();
    check_order(order, {"number.begin", "text.work", "owned.begin", "void.begin",
                        "number.end", "owned.end", "void.end"});
    check(number->result() == 42, "loop did not preserve the final int result");
    check(text->result() == "done"s, "loop did not preserve the string result");
    auto value = owned->result();
    check(value && *value == 8, "loop did not preserve the move-only final result");
    nothing->result();
    rejected = false;
    try { nothing->result(); }
    catch (const yq::CoroutineException&) { rejected = true; }
    check(rejected, "void task result could be consumed twice");
    rejected = false;
    try { static_cast<void>(number->result()); }
    catch (const yq::CoroutineException&) { rejected = true; }
    check(rejected, "task result could be consumed twice");
    number.reset();
    check(observed.expired(), "loop retained a finished task");
    check_root();
}

void sleep_in_ordinary_function() {
    yq::co::sleep(20ms);
}

void test_loop_sleep_and_other_work() {
    auto& loop = yq::CoLoop::instance();
    std::vector<std::string> order;
    std::chrono::steady_clock::duration elapsed{};
    auto slow = loop.spawn([&order, &elapsed] {
        order.push_back("slow.begin");
        const auto before = std::chrono::steady_clock::now();
        sleep_in_ordinary_function();
        elapsed = std::chrono::steady_clock::now() - before;
        order.push_back("slow.end");
        return 1;
    });
    loop.spawn([&order] { order.push_back("fast.work"); });
    loop.run();
    check_order(order, {"slow.begin", "fast.work", "slow.end"});
    check(elapsed >= 20ms && slow->result() == 1, "sleep woke early or lost the task result");
}

void test_loop_spawn_and_await() {
    auto& loop = yq::CoLoop::instance();
    std::vector<std::string> order;
    auto parent = loop.spawn([&order] {
        order.push_back("parent.begin");
        auto child = yq::CoLoop::instance().spawn([&order] {
            order.push_back("child.begin");
            yq::co::sleep(0ms);
            order.push_back("child.end");
            return 42;
        });
        order.push_back("parent.wait");
        const int value = child->await();
        order.push_back("parent.got-child");
        check(value == 42, "await lost the child's result");
        return "parent done"s;
    });
    loop.spawn([&order] { order.push_back("sibling.work"); });
    loop.run();
    check(parent->result() == "parent done"s, "await lost the parent's final result");
    check_order(order, {"parent.begin", "parent.wait", "sibling.work",
                        "child.begin", "child.end", "parent.got-child"});
    check_root();
}

void test_loop_await_void_and_move_only() {
    auto& loop = yq::CoLoop::instance();
    auto owned = loop.spawn([] {
        yq::co::sleep(0ms);
        return std::make_unique<int>(7);
    });
    bool work_done = false;
    auto work = loop.spawn([&work_done] {
        yq::co::sleep(0ms);
        work_done = true;
    });
    auto parent = loop.spawn([owned, work, &work_done] {
        auto value = owned->await();
        work->await();
        check(work_done && value && *value == 7, "await did not handle void or move-only results");
        return *value;
    });
    loop.run();
    check(parent->await() == 7, "await of an already finished task failed at root");
}

void test_loop_await_cycle() {
    auto& loop = yq::CoLoop::instance();
    std::vector<std::string> order;
    std::shared_ptr<yq::CoTask<int>> a;
    std::shared_ptr<yq::CoTask<int>> b;
    a = loop.spawn([&b, &order] {
        order.push_back("A.wait-B");
        int value = b->await();
        order.push_back("A.got-B");
        return value + 1;
    });
    b = loop.spawn([&a, &order] {
        bool rejected = false;
        try { static_cast<void>(a->await()); }
        catch (const yq::CoroutineException&) {
            rejected = true;
            order.push_back("B.cycle-rejected");
        }
        check(rejected, "await cycle was not rejected");
        return 2;
    });
    loop.run();
    check(a->result() == 3, "rejecting the await cycle broke the waiting task");
    check_order(order, {"A.wait-B", "B.cycle-rejected", "A.got-B"});
}

void test_loop_cancel_and_wake_waiter() {
    auto& loop = yq::CoLoop::instance();
    std::vector<std::string> order;
    bool destroyed = false;
    auto target = loop.spawn([&destroyed, &order] {
        TaskLocal local{destroyed};
        order.push_back("target.sleep");
        yq::co::sleep(1h);
        return 42;
    });
    auto waiter = loop.spawn([target, &order] {
        order.push_back("waiter.wait");
        bool rejected = false;
        try { static_cast<void>(target->await()); }
        catch (const yq::CoroutineException&) {
            rejected = true;
            order.push_back("waiter.cancel-received");
        }
        check(rejected, "await of a cancelled task returned a result");
        return 7;
    });
    loop.spawn([target, &order] {
        order.push_back("other.cancel-target");
        target->cancel();
    });
    loop.run();
    check(destroyed && target->is_finished() && waiter->result() == 7,
          "cancellation did not unwind the task or wake its waiter");
    check_order(order, {"target.sleep", "waiter.wait", "other.cancel-target", "waiter.cancel-received"});
    check_root();
}

void test_loop_invalid_operations() {
    auto& loop = yq::CoLoop::instance();
    bool rejected = false;
    try { yq::co::sleep(0ms); }
    catch (const yq::CoroutineException&) { rejected = true; }
    check(rejected, "sleep outside the loop was accepted");
    auto target = loop.spawn([] { return 42; });
    rejected = false;
    try { static_cast<void>(target->await()); }
    catch (const yq::CoroutineException&) { rejected = true; }
    check(rejected, "root awaited an unfinished task without running the loop");

    std::shared_ptr<yq::CoTask<void>> self;
    self = loop.spawn([&self, &loop] {
        bool self_rejected = false;
        try { self->await(); }
        catch (const yq::CoroutineException&) { self_rejected = true; }
        check(self_rejected, "task was allowed to await itself");
        bool run_rejected = false;
        try { loop.run(); }
        catch (const yq::CoroutineException&) { run_rejected = true; }
        check(run_rejected, "recursive run was accepted");
        // 手动创建的内层协程不能修改循环任务的 sleep 状态。
        yq::Coroutine<void> inner([] {
            bool sleep_rejected = false;
            try { yq::co::sleep(1h); }
            catch (const yq::CoroutineException&) { sleep_rejected = true; }
            check(sleep_rejected, "unmanaged inner coroutine used the loop's sleep");
        });
        inner.resume();
        yq::co::sleep(0ms);
    });
    bool never_started = false;
    auto cancelled = loop.spawn([&never_started] { never_started = true; });
    cancelled->cancel();
    loop.run();
    check(!never_started && self->is_finished(), "invalid operations changed task execution");
    check(target->result() == 42, "rejected root await broke the pending task");
}

void test_loop_exception_cleanup_and_reuse() {
    auto& loop = yq::CoLoop::instance();
    struct TaskError : std::runtime_error {
        TaskError() : std::runtime_error("loop task failed") {}
    };
    bool destroyed = false;
    bool after_sleep = false;
    bool queued_ran = false;
    auto paused = loop.spawn([&destroyed, &after_sleep] {
        TaskLocal local{destroyed};
        yq::co::sleep(1h);
        after_sleep = true;
        return 7;
    });
    auto failed = loop.spawn([]() -> int { throw TaskError{}; });
    auto queued = loop.spawn([&queued_ran] { queued_ran = true; });
    bool caught = false;
    try { loop.run(); }
    catch (const TaskError&) { caught = true; }
    check(caught && destroyed && !after_sleep && !queued_ran,
          "loop exception lost its type or failed to cancel the remaining tasks");
    check(paused->is_finished() && failed->is_finished() && queued->is_finished(),
          "loop cleanup left tasks resumable");
    caught = false;
    try { static_cast<void>(failed->result()); }
    catch (const TaskError&) { caught = true; }
    check(caught, "failed task did not retain its original exception");
    bool cancelled_result_rejected = false;
    try { static_cast<void>(paused->result()); }
    catch (const yq::CoroutineException&) { cancelled_result_rejected = true; }
    check(cancelled_result_rejected, "cancelled task exposed a result");
    check_root();
    loop.run(); // 空循环。
    int source = 3;
    auto next = loop.spawn([](int& n) { return ++n; }, std::ref(source));
    loop.run();
    check(next->result() == 4 && source == 4, "loop could not run a new batch or preserve std::ref");
}

void test_coroutine(auto& test) {
    std::cout << "\n=== Coroutine tests ===\n";
    test("void yield across an ordinary function", test_void_yield_across_function);
    test("pause values and final value", test_pause_and_final_values);
    test("type mismatch and string values", test_type_mismatch_and_string_values);
    test("nested coroutines with different result types", test_nested_coroutines);
    test("move-only pause and final values", test_move_only_results);
    test("cancellation unwinds task locals and rejects a new yield", test_cancellation_cleanup);
    test("root yield rejection and task exception", test_root_yield_and_task_exception);
}

void test_coroutine_loop(auto& test) {
    std::cout << "\n=== Coroutine loop tests ===\n";
    test("loop typed results and execution order", test_loop_typed_results_and_order);
    test("loop sleep and other work", test_loop_sleep_and_other_work);
    test("spawn a child and await its result", test_loop_spawn_and_await);
    test("await void and move-only results", test_loop_await_void_and_move_only);
    test("reject await cycles", test_loop_await_cycle);
    test("cancel a sleeping task and wake its waiter", test_loop_cancel_and_wake_waiter);
    test("loop invalid operations", test_loop_invalid_operations);
    test("loop exception cleanup and reuse", test_loop_exception_cleanup_and_reuse);
}

} // namespace

int main() {
    int passed = 0;
    int failed = 0;
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
            std::cout << "[FAIL] " << name << ": unexpected exception\n";
        }
    };
    test_coroutine(test);
    test_coroutine_loop(test);
    std::cout << "\nResult: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
