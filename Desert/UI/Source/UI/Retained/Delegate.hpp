#pragma once

#include <functional>
#include <type_traits>
#include <utility>

// A SINGLE-CAST CALLBACK, as Slate's TDelegate is: bound or not, and executed only when bound. The retained
// widget tree (WidgetTree.hpp) keeps one per widget event — a button's click, a slider's value change — and
// fires it after the frame the event happened in, from WidgetTree::DispatchFrame.
//
// A thin wrapper over std::function on purpose: what it adds is the vocabulary (IsBound / ExecuteIfBound),
// so a call site reads as "fire the widget's event if anyone listens" rather than as a null check.
namespace Desert::UI
{
    template <class Signature>
    class Delegate;

    template <class R, class... Args>
    class Delegate<R( Args... )>
    {
    public:
        Delegate() = default;

        template <class F>
            requires std::is_invocable_r_v<R, F&, Args...> && (!std::is_same_v<std::decay_t<F>, Delegate>)
        Delegate( F&& fn ) // NOLINT(google-explicit-constructor): a lambda IS the delegate it binds
             : m_Fn( std::forward<F>( fn ) )
        {
        }

        [[nodiscard]] bool IsBound() const
        {
            return static_cast<bool>( m_Fn );
        }

        void Unbind()
        {
            m_Fn = nullptr;
        }

        // Call the bound function. Only when IsBound(): an unbound delegate has no answer to return.
        R Execute( Args... args ) const
        {
            return m_Fn( std::forward<Args>( args )... );
        }

        // Call it when bound; do nothing otherwise. Events have no answer, so only a void delegate has this.
        void ExecuteIfBound( Args... args ) const
            requires std::is_void_v<R>
        {
            if ( m_Fn )
                m_Fn( std::forward<Args>( args )... );
        }

    private:
        std::function<R( Args... )> m_Fn;
    };
} // namespace Desert::UI
