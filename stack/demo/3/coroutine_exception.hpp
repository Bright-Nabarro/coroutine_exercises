#pragma once

#include <stdexcept>

namespace yq {

// 调用方违反协程接口的使用约定时抛出。
class CoroutineException : public std::logic_error {
public:
    using std::logic_error::logic_error;
};

class Cancelled{};

} // namespace yq
