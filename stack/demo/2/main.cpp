#include <print>
#include <string>
#include "yq_coroutine.hpp"

using namespace yq;

// 测试1: 基础功能测试和异常测试
void test_basic_and_exception() {
    std::println("=== Test 1: Basic and Exception ===");
    
    int counter = 0;
    
    Coroutine co([&counter]() {
        std::println("Coroutine start");
        counter++;
        Coroutine::yield();
        std::println("Coroutine resume");
        counter++;
        Coroutine::yield();
        std::println("Coroutine end");
        counter++;
    });
    
    // 第一次 resume
    assert(counter == 0);
    assert(!co.is_finished());
    co.resume();    // 协程开始
    assert(counter == 1);
    assert(!co.is_finished());
    
    // 第二次 resume
    co.resume();
    assert(counter == 2);
    assert(!co.is_finished());
    
    // 第三次 resume，协程结束
    co.resume();
    assert(counter == 3);
    assert(co.is_finished());
    
    // 测试异常：对已结束的协程调用 resume
    bool exception_caught = false;
    try {
        co.resume();
    } catch (const std::logic_error& e) {
        exception_caught = true;
        std::println("Caught expected exception: {}", e.what());
    }
    assert(exception_caught);
    
    std::println("Test 1 passed!\n");
}
 
// 测试2: 同一个协程的嵌套函数中的 yield
void test_nested_yield_same_coroutine() {
    std::println("=== Test 2: Nested Yield in Same Coroutine ===");
    
    int step = 0;
    
    auto nested_func = [&step]() {
        std::println("  Nested function start, step = {}", step);
        step++;
        Coroutine::yield();
        std::println("  Nested function end, step = {}", step);
        step++;
    };
    
    Coroutine co([&step, &nested_func]() {
        std::println("Coroutine start, step = {}", step);
        step++;
        Coroutine::yield();
        
        std::println("Coroutine calling nested function, step = {}", step);
        step++;
        nested_func();  // 嵌套函数中也会 yield
        
        std::println("Coroutine end, step = {}", step);
        step++;
    });
    
    assert(step == 0);
    co.resume();  // step: 0 -> 1, yield
    assert(step == 1);
    
    co.resume();  // step: 1 -> 2 -> 3, yield in nested_func
    assert(step == 3);
    
    co.resume();  // step: 3 -> 4 -> 5, coroutine ends
    assert(step == 5);
    assert(co.is_finished());
    
    std::println("Test 2 passed!\n");
}
 
// 测试3: 不同协程的嵌套 yield
void test_nested_yield_different_coroutines() {
    std::println("=== Test 3: Nested Yield in Different Coroutines ===");
    
    int step = 0;
    
    // 内层协程
    auto inner_coroutine_task = [&step]() {
        std::println("  Inner coroutine start, step = {}", step);
        step++;  // step 变为 3
        Coroutine::yield();
        std::println("  Inner coroutine middle, step = {}", step);
        step++;  
        Coroutine::yield();
        std::println("  Inner coroutine end, step = {}", step);
        step++;  
    };
    
    // 外层协程
    auto outer_coroutine_task = [&step, &inner_coroutine_task]() {
        std::println("Outer coroutine start, step = {}", step);
        step++;  // step 变为 1
        Coroutine::yield();     // outer yield 1
        
        std::println("Outer coroutine creating inner coroutine, step = {}", step);
        step++;  // step 变为 2
        
        Coroutine inner_co(inner_coroutine_task);
        
        std::println("Outer coroutine resuming inner coroutine first time, step = {}", step);
        inner_co.resume();  // 内层执行: step 2->3, 然后 yield
        assert(step == 3);
        
        std::println("Outer coroutine yielding, step = {}", step);
        Coroutine::yield();  // out yield 2
        
        std::println("Outer coroutine resuming inner coroutine second time, step = {}", step);
        inner_co.resume();  
        assert(step == 4);
        
        std::println("Outer coroutine yielding again, step = {}", step);
        Coroutine::yield();  // out yield 3
        
        std::println("Outer coroutine resuming inner coroutine third time, step = {}", step);
        inner_co.resume();  // 
        assert(step == 5);
        assert(inner_co.is_finished());
        
        std::println("Outer coroutine end, step = {}", step);
        step++;  // 
    };
    
    Coroutine outer_co(outer_coroutine_task);
    
    assert(step == 0);
    outer_co.resume();  // start -> yield 1
    assert(step == 1);
    
    outer_co.resume();  // yield 1 -> yield 2
    assert(step == 3);
    
    outer_co.resume();  // yield 2 -> yield 3
    assert(step == 4);
    
    outer_co.resume();  // yield 3 -> end
    assert(step == 6);
    assert(outer_co.is_finished());
    
    std::println("Test 3 passed!\n");
}

void test_variable_parameters() {
    std::println("=== Test 4: Single variable parameter callback ===");
    VarCoroutine<int> co([](int a) {
        std::println("callback got a: {}", a);
        ++a;
        Coroutine::yield();
        std::println("calback increase a: {}", a);
    });
    
    while(!co.is_finished()) {
        co.resume();
    }

    std::println("Test 4 passed!\n");
}

void test_nested_variable_parameters() {
    std::println("=== Test 5: muitable variable parameter callback ===");
    auto inner = [](int a, int b, int c) {
        VarCoroutine<std::string> co([](std::string str) {
            std::println("inner get {}", str);
            std::reverse(str.begin(), str.end());
            Coroutine::yield();
            std::println("inner reverse {}", str);
            std::reverse(str.begin(), str.end());
            Coroutine::yield();
            std::println("inner reverse {}", str);
        }, "hello world");

        std::println("inner begin");
        std::println("a {}", a);
        Coroutine::yield();
        std::println("{} {}", b, c);
        while(!co.is_finished()) {
            co.resume();
        }
    };
    VarCoroutine<int, int, int> co(inner, 1, 2, 3);
    while(!co.is_finished()) {
        co.resume();
    }
    std::println("Test 5 passed\n");
}
 
// 主测试函数
void run_all_tests() {
    test_basic_and_exception();
    test_nested_yield_same_coroutine();
    test_nested_yield_different_coroutines();
    test_variable_parameters();
    test_nested_variable_parameters();
    std::println("=== All tests passed! ===");
}
 
auto main() -> int {
    try {
        run_all_tests();
    } catch (const std::exception& e) {
        std::println("Test failed with exception: {}", e.what());
        return 1;
    }
    return 0;
}