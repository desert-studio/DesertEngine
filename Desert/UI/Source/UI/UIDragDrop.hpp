#pragma once

#include <any>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <glm/glm.hpp>

// THE TYPED DRAG-AND-DROP OPERATION — Slate's FDragDropOperation, without the class hierarchy.
//
// A drag carries one UIDragPayload from the press that starts it to the release that drops it: a TYPE id
// (what a target asks "do I take this"), the DATA it owns (an asset id, a list row, an entity — anything
// copyable, held by value in std::any so the drag keeps it alive however long the pointer wanders), and the
// DECORATOR the cursor shows (Slate's FDecoratedDragDropOp: a label beside a ghost of the source).
//
// IN UE THE TYPE IS A C++ CLASS (DRAG_DROP_OPERATOR_TYPE + IsOfType) AND THE DATA ARE ITS MEMBERS. Here the
// type is a name and the data an std::any: a target filters on a string the author can read in a log and a
// scenario, the authored (ECS) layer can state one without C++ (UIDraggableData), and As<T>() is still a
// checked cast — a payload of the right name with the wrong data type answers nullptr instead of
// reinterpreting bytes. What UE gets from the class — virtual OnDrop/OnDragged on the operation itself —
// belongs to the TARGET here (IUITree::DropAnswerOf, WidgetTree::OnDrop), which is where a drop's meaning
// lives: the same "asset" payload means "assign" to a property row and "place" to a viewport.
//
// Who answers what (UICanvasRenderer2D.cpp, EndUIFrame):
//   * the SOURCE makes the payload on the press (IUITree::DragSourceOf, or an authored UIDraggableData);
//   * every TARGET answers each payload in flight (UIDropAnswer) — the walk outlines the ones that accept,
//     and the one under the cursor that refuses shows the rejected outline and the decorator turns red;
//   * the release over an accepting target is a DROP (UIViewContext::LastDrop, read by the tree's
//     DispatchFrame); anywhere else — over a refusing target, over nothing — and Escape at any time CANCEL:
//     the payload is destroyed and nobody hears it.
namespace Desert::UI
{
    // The payload type an AUTHORED drag (UIDraggableData) carries: its Data is the authored std::string
    // ("item:sword"), filtered by an authored target's prefix (UIDropTargetData::Accepts).
    inline constexpr std::string_view kAuthoredDragType = "ui.authored";

    // What the cursor shows while the drag is in flight.
    struct UIDragDecorator
    {
        std::string Label;           // drawn beside the ghost; empty = ghost only
        glm::vec2   Size{ 0.0f };    // the ghost's footprint (px); zero = the source's own rect
        float       Opacity = 0.55f; // the ghost's alpha
    };

    class UIDragPayload
    {
    public:
        UIDragPayload() = default;

        template <class T>
        [[nodiscard]] static UIDragPayload Make( std::string type, T data, UIDragDecorator decorator = {} )
        {
            UIDragPayload p;
            p.m_Type      = std::move( type );
            p.m_Data      = std::move( data );
            p.m_Decorator = std::move( decorator );
            return p;
        }

        // No payload at all — what a drag state holds between drags.
        [[nodiscard]] bool Empty() const
        {
            return m_Type.empty();
        }
        [[nodiscard]] const std::string& Type() const
        {
            return m_Type;
        }
        [[nodiscard]] bool Is( std::string_view type ) const
        {
            return !m_Type.empty() && m_Type == type;
        }
        // The data as T, or nullptr when it holds another type (or none).
        template <class T>
        [[nodiscard]] const T* As() const
        {
            return std::any_cast<T>( &m_Data );
        }
        [[nodiscard]] const UIDragDecorator& Decorator() const
        {
            return m_Decorator;
        }
        [[nodiscard]] UIDragDecorator& Decorator()
        {
            return m_Decorator;
        }

    private:
        std::string     m_Type;
        std::any        m_Data;
        UIDragDecorator m_Decorator;
    };

    // A target's answer to the payload in flight (Slate's OnDragOver reply).
    enum class UIDropAnswer : std::uint8_t
    {
        None,   // not a drop target for anything: the pointer passes through to its ancestors
        Accept, // takes it — outlined while the drag is up, and the release drops here
        Reject  // a target, but not for this — the rejected outline under the cursor; a release cancels
    };
} // namespace Desert::UI
