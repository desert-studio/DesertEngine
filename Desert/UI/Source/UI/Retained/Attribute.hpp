#pragma once

#include <functional>
#include <type_traits>
#include <utility>

// A VALUE THAT IS EITHER SET OR BOUND — Slate's TAttribute. A widget's label, a slider's value, a panel's
// colour can be a constant written once when the widget is built, or a getter the tree asks every frame
// (WidgetTree::RefreshAttributes, before the frame is walked), so a widget shows the model it is bound to
// without anyone pushing every change into it.
//
// The tree the framework walks stays a VIEW (UITree.hpp): a bound attribute is evaluated into the node's
// authored arguments before the walk, and the walk reads those arguments exactly as it reads the ECS scene's.
namespace Desert::UI
{
    template <class T>
    class Attribute
    {
    public:
        Attribute() = default;

        // A constant — from anything a T is made from, so a string literal sets a string attribute.
        template <class U>
            requires std::is_convertible_v<U, T> && ( !std::is_same_v<std::remove_cvref_t<U>, Attribute> )
        Attribute( U&& value ) // NOLINT(google-explicit-constructor): a plain value IS a set attribute
             : m_Value( std::forward<U>( value ) )
        {
        }

        // A getter, asked on every Get().
        template <class F>
            requires std::is_invocable_r_v<T, F&>
        [[nodiscard]] static Attribute Bound( F&& getter )
        {
            Attribute a;
            a.m_Getter = std::forward<F>( getter );
            return a;
        }

        [[nodiscard]] bool IsBound() const
        {
            return static_cast<bool>( m_Getter );
        }

        [[nodiscard]] T Get() const
        {
            return m_Getter ? m_Getter() : m_Value;
        }

    private:
        T                  m_Value{};
        std::function<T()> m_Getter;
    };
} // namespace Desert::UI
