#include "co_loop.hpp"
#include <chrono>
#include <exception>
#include <iostream>
#include <string>

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
Clock::time_point started_at;

void log(const char* worker, const char* message, int step = 0) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - started_at).count();
    std::cout << '[' << elapsed << " ms] " << worker << ": " << message;
    if (step != 0) {
        std::cout << ' ' << step;
    }
    std::cout << std::endl;
}

// 这是协程的 sleep：暂停当前任务，让循环去运行其他任务。
void sleep(std::chrono::milliseconds delay) {
    yq::CoLoop::instance().sleep(delay);
}

int work(const char* name, int steps, std::chrono::milliseconds interval) {
    for (int step = 1; step <= steps; ++step) {
        log(name, "work step", step);
        // 这里可以做计算、更新状态等；普通函数里也可以暂停。
        if (step != steps) {
            log(name, "sleep");
            sleep(interval);
            log(name, "wake up");
        }
    }
    log(name, "finished");
    return steps;
}

} // namespace

int main() {
    started_at = Clock::now();
    auto& loop = yq::CoLoop::instance();

    // 两个任务在同一个线程执行，休眠时交替工作。
    auto a = loop.spawn([] { return work("A", 3, 200ms); });
    auto b = loop.spawn([] {
        work("B", 4, 100ms);
        return std::string{"B completed"};
    });

    log("root", "run loop");
    try {
        loop.run();
    } catch (const std::exception& error) {
        std::cerr << "task failed: " << error.what() << '\n';
        return 1;
    }
    log("root", "all tasks finished");
    // a 是 shared_ptr<CoTask<int>>，b 是 shared_ptr<CoTask<string>>。
    // 循环移除任务后，这两个句柄仍然保存结果，不需要 dynamic_cast。
    std::cout << "A result: " << a->coroutine()->result() << '\n';
    std::cout << "B result: " << b->coroutine()->result() << '\n';
}
