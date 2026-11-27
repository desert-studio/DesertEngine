// BuildPool contract: errors are values. No function of the pool throws; every failure carries a reason that
// reaches the person who started the build (the client prints it as "buildpool: <message>" on stderr).
// CONTRACT HEADER: written by the architect, edited by no package.
#pragma once

#include <string>
#include <utility>
#include <variant>

namespace buildpool
{
    struct Error
    {
        std::string message;
    };

    inline Error Fail(std::string message) { return Error{std::move(message)}; }

    // Either a T or the reason there is none. T is never Error.
    template <class T>
    class [[nodiscard]] Result
    {
    public:
        Result(T value) : m_state(std::in_place_index<0>, std::move(value)) {}
        Result(Error error) : m_state(std::in_place_index<1>, std::move(error)) {}

        explicit operator bool() const noexcept { return m_state.index() == 0; }

        // Precondition for value()/take(): bool(*this). Precondition for error(): !bool(*this).
        T& value() & noexcept { return *std::get_if<0>(&m_state); }
        const T& value() const& noexcept { return *std::get_if<0>(&m_state); }
        T take() && noexcept { return std::move(*std::get_if<0>(&m_state)); }
        const Error& error() const noexcept { return *std::get_if<1>(&m_state); }

    private:
        std::variant<T, Error> m_state;
    };

    struct Unit
    {
    };
    using Status = Result<Unit>;
    inline Status Ok() { return Unit{}; }
} // namespace buildpool
