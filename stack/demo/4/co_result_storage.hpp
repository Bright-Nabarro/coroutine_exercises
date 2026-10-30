#pragma once

#include <optional>
#include <type_traits>
#include <utility>

namespace yq {

// 只保存值；暂停值和最终值共用一个槽位，取出后清空。
template<class R>
class ResultStorage {
    static_assert(!std::is_reference_v<R>, "R must be a value type");

public:
    void emplace(R value) {
        m_value.emplace(std::move(value));
    }

    R forward() {
        R value = std::move(m_value.value());
        m_value.reset();
        return value;
    }

	bool has_value() const noexcept {
		return m_value.has_value();
	}

private:
    std::optional<R> m_value;
};

template<>
class ResultStorage<void> {
public:
    void emplace() noexcept {}
    void forward() noexcept {}
	bool has_value() const noexcept { return false; }

};

} // namespace yq
