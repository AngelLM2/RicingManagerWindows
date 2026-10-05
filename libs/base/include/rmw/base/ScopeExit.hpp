#pragma once

#include <type_traits>
#include <utility>

namespace rmw
{

// Runs a noexcept callable when leaving the scope, unless release() was called.
//   auto cleanup = rmw::ScopeExit([&]() noexcept { removeTempDir(); });
template <class F>
class [[nodiscard]] ScopeExit
{
    static_assert(std::is_nothrow_invocable_v<F&>, "ScopeExit callables must be noexcept");

public:
    explicit ScopeExit(F callable) noexcept(std::is_nothrow_move_constructible_v<F>)
        : m_callable(std::move(callable))
    {
    }

    ~ScopeExit()
    {
        if (m_active)
        {
            m_callable();
        }
    }

    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ScopeExit(ScopeExit&&) = delete;
    ScopeExit& operator=(ScopeExit&&) = delete;

    void release() noexcept { m_active = false; }

private:
    F m_callable;
    bool m_active = true;
};

} // namespace rmw
