#pragma once
#include "yq_coroutine.hpp"
#include <chrono>

namespace yq
{

struct CoTaskBase
{

    using Clock = std::chrono::steady_clock;
	virtual ~CoTaskBase() = default;
    Clock::time_point wake_at { Clock::time_point::min() };
	virtual std::shared_ptr<CoroutineBase> base_coroutine() = 0;
};

template<typename R>
struct CoTask: public CoTaskBase
{
	friend class CoLoop;
explicit CoTask(std::shared_ptr<Coroutine<R>> co):
	m_coroutine { std::move(co) }
	{
	}


	std::shared_ptr<CoroutineBase> base_coroutine() override
	{
		return m_coroutine;
	}

	std::shared_ptr<Coroutine<R>> coroutine() const
	{
		return m_coroutine;
	}

	R await()
	{
		
	}

	std::shared_ptr<Coroutine<R>> m_coroutine;
};

}
