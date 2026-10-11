#include "../result_storage.hpp"
#include <iostream>
#include <memory>
#include <stdexcept>

#define CHECK(c) do { if (!(c)) throw std::runtime_error("check failed: " #c); } while (false)

struct Tracked {
    static inline int alive = 0;
    int value;
    explicit Tracked(int n) : value(n) { ++alive; }
    Tracked(const Tracked&) = delete;
    Tracked(Tracked&& other) noexcept : value(other.value) { ++alive; }
    ~Tracked() { --alive; }
};

int main() {
    try {
        {
            yq::ResultStorage<Tracked> result;
            CHECK(Tracked::alive == 0);
            result.emplace(Tracked(10));
            CHECK(Tracked::alive == 1);
            auto value = result.forward();
            CHECK(value.value == 10 && Tracked::alive == 1);
            bool caught = false;
            try { static_cast<void>(result.forward()); }
            catch (const std::bad_optional_access&) { caught = true; }
            CHECK(caught);
        }
        CHECK(Tracked::alive == 0);
        {
            yq::ResultStorage<std::unique_ptr<int>> result;
            result.emplace(std::make_unique<int>(42));
            auto pointer = result.forward();
            CHECK(*pointer == 42);
            result.emplace(std::make_unique<int>(43));
            CHECK(*result.forward() == 43);
        }
        {
            // 任务完成前可以反复保存/取走暂停值，最终值随后才写入。
            yq::ResultStorage<int> result;
            auto task = [&] {
                result.emplace(1);
                CHECK(result.forward() == 1);
                result.emplace(2);
                CHECK(result.forward() == 2);
                return 3;
            };
            result.emplace(task());
            CHECK(result.forward() == 3);
        }
        {
            yq::ResultStorage<void> result;
            result.emplace();
            result.forward();
            result.emplace();
            result.forward();
        }
        std::cout << "ResultStorage value/void tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
