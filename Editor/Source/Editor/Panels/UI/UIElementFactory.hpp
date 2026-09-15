#pragma once

#include <Editor/Core/Commands/SceneCommands.hpp>
#include <Editor/Panels/UI/UIElementCatalog.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/Entity.hpp>

#include <Engine/UI/UIOverlay.hpp>
#include <Engine/UI/UICanvasLayout.hpp>

#include <Editor/Core/Selection/SelectionManager.hpp>

#include <Common/Core/ResultStr.hpp>

#include <entt/entt.hpp>

#include <cstddef>
#include <string>

// The one implementation of "make a UI element". There used to be two — UIEditorPanel.cpp and
// ViewportPanel.cpp each carried a private AddUIChild template — and the two menus that called them had
// already drifted apart. Both call this now, so an element created in the panel and the same element
// created in the viewport are the same entity by construction rather than by careful copying.
//
// AND IT IS ALSO WHERE THE CREATION IS RECORDED. Every other creator in the editor — the Add menu, the
// prefab drop, the viewport mesh drop, the file explorer — calls Commands::NotifyCreated so Ctrl+Z can
// take the entity back; the two UI menus were the only ones that did not, so a UI element, once added,
// could only be removed by finding it in the outliner and deleting it by hand. Putting the call inside
// the factory rather than beside each menu item is the point: a third UI menu cannot be written without
// it, which is exactly how the two menus drifted apart the first time.
namespace Desert::Editor
{
    // FindUICanvas USED TO LIVE HERE, and it was the third copy of `*reg.view<UICanvasComponent>().begin()`
    // — "the scene's canvas (the first one, which is the one the renderer draws)". Its own comment recorded
    // the coincidence as a rule. It is gone: ask ::Desert::UI::CanvasOf for the canvas an element belongs to,
    // or ::Desert::UI::SoleCanvas when there is genuinely nothing else to go on, and get a named refusal
    // instead of a winner when the scene has more than one.

    // WHERE THE NEXT UI THING GOES, asked once.
    //
    // The viewport's create menu worked this out inline, and then the prefab drop needed the same answer:
    // two derivations of "which canvas, and which element inside it" would have differed the first time
    // either was touched, and the symptom would be an element created in a canvas the author is not
    // looking at. The refusal is a value rather than a log line because the menu shows it as a tooltip
    // and the drop logs it — one sentence, two presentations.
    //
    // The selection answers it EXACTLY when there is one (an element names its own canvas); only a scene
    // with a single canvas has an answer without it. Two canvases and nothing selected is genuinely no
    // answer, and saying so is not the same as picking the first.
    inline Common::ResultStr<entt::entity> UICanvasForCreate( ::Desert::Core::Scene& scene )
    {
        auto& reg = scene.GetRegistry();
        if ( const auto& sel = Core::SelectionManager::GetSelected(); sel.has_value() )
        {
            if ( auto ref = scene.FindEntityByID( *sel ) )
            {
                const entt::entity canvas = ::Desert::UI::CanvasOf( reg, ref->get().GetHandle() );
                if ( canvas != entt::null )
                {
                    return Common::MakeSuccess( canvas );
                }
            }
        }
        return ::Desert::UI::SoleCanvas( reg );
    }

    // The parent INSIDE @p canvas: the selected element when the selection is one, the canvas otherwise.
    // Nesting under what the author has selected is what makes "add a button to this panel" mean what it
    // reads as.
    inline entt::entity UIParentForCreate( ::Desert::Core::Scene& scene, entt::entity canvas )
    {
        auto& reg = scene.GetRegistry();
        if ( const auto& sel = Core::SelectionManager::GetSelected(); sel.has_value() )
        {
            if ( auto ref = scene.FindEntityByID( *sel ) )
            {
                const entt::entity h = ref->get().GetHandle();
                if ( h == canvas || reg.has<ECS::UILayoutComponent>( h ) )
                {
                    return h;
                }
            }
        }
        return canvas;
    }

    // Create a UI child entity (a UILayout plus @p ElementComponent) parented to @p parent, and return its
    // handle so the caller can select it. The UILayout is not optional: it is the rect the renderer resolves
    // anchors into, and an element without one is laid out as its parent and cannot be picked or dragged.
    template <typename ElementComponent>
    entt::entity AddUIChild( ::Desert::Core::Scene& scene, entt::entity parent, const char* name )
    {
        auto& e      = scene.CreateNewEntity( std::string( name ) );
        auto  handle = e.GetHandle();
        e.AddComponent<ECS::UILayoutComponent>();
        e.AddComponent<ElementComponent>();

        auto& reg = scene.GetRegistry();
        if ( !reg.has<ECS::RelationshipComponent>( handle ) )
            reg.emplace<ECS::RelationshipComponent>( handle );
        reg.get<ECS::RelationshipComponent>( handle ).Parent = parent;
        if ( !reg.has<ECS::RelationshipComponent>( parent ) )
            reg.emplace<ECS::RelationshipComponent>( parent );
        reg.get<ECS::RelationshipComponent>( parent ).Children.push_back( handle );
        return handle;
    }

    // Records an entity this file just created as one undo step, and hands the handle back so the caller
    // can select it. Null in, null out — a creator that failed has nothing to record.
    inline entt::entity RecordUICreation( ::Desert::Core::Scene& scene, entt::entity handle )
    {
        if ( handle == entt::null )
            return handle;

        auto& reg = scene.GetRegistry();
        if ( reg.has<ECS::UUIDComponent>( handle ) )
            Commands::NotifyCreated( { reg.get<ECS::UUIDComponent>( handle ).UUID } );
        return handle;
    }

    // Create the canvas a UI tree hangs from. It is its own function rather than three lines in the
    // viewport toolbar because it is a creation like any other and has to be recorded like one — the
    // toolbar's copy was not, so the very first thing a user does when authoring UI was the one step
    // Ctrl+Z could not take back.
    inline entt::entity CreateUICanvas( ::Desert::Core::Scene& scene )
    {
        auto& e = scene.CreateNewEntity( "UI Canvas" );
        e.AddComponent<ECS::UICanvasComponent>();
        return RecordUICreation( scene, e.GetHandle() );
    }

    // ------------------------------------------------------------------------------------------------
    // OVERLAYS (Ю12): the four kinds, created WITH THEIR CHROME.
    //
    // An overlay is a canvas, and a bare canvas with a UIOverlayComponent on it shows nothing — the chrome
    // (the tooltip's panel and label, the menu's items, the dialog and its buttons, the toast slots) is
    // ordinary authored UI. Offering the component without the chrome would hand the author an empty
    // rectangle and a puzzle, so the menu entry builds a working example that can then be edited like any
    // other tree. This is also where the two authoring conventions the runtime depends on are established:
    // a tooltip label binds `overlay.text`, and toast slot N binds `overlay.toast.N.text` / `.visible`.
    // ------------------------------------------------------------------------------------------------

    // One text element with a size, a colour and (optionally) a data binding. Returns its handle.
    inline entt::entity AddOverlayText( ::Desert::Core::Scene& scene, entt::entity parent, const char* name,
                                        const char* text, const glm::vec2& min, const glm::vec2& max,
                                        float fontSize, const char* bindKey = nullptr )
    {
        const entt::entity h   = AddUIChild<ECS::UITextComponent2D>( scene, parent, name );
        auto&              reg = scene.GetRegistry();
        auto&              L   = reg.get<ECS::UILayoutComponent>( h ).Data;
        L.AnchorMin            = { 0.0f, 0.0f };
        L.AnchorMax            = { 0.0f, 0.0f };
        L.OffsetMin            = min;
        L.OffsetMax            = max;
        L.HitTest              = ECS::UIHitTest::None; // a label never takes a click from the item under it
        auto& T                = reg.get<ECS::UITextComponent2D>( h ).Data;
        T.Text                 = text;
        T.FontSize             = fontSize;
        T.VerticalAlign        = ECS::UITextVAlign::Middle;
        if ( bindKey != nullptr )
        {
            auto& B  = reg.emplace<ECS::UIBindingComponent>( h ).Data;
            B.Key    = bindKey;
            B.Target = ECS::UIBindTarget::Text;
        }
        return h;
    }

    // One context-menu row: a button, its label, and the accelerator it displays on the right.
    //
    // THE ACCELERATOR IS A LABEL AND NOT A BINDING, and that is a limit of the input layer rather than a
    // shortcut taken here: UI::UIInput carries typed text, Backspace, Tab, Enter and Escape and no key
    // codes at all, so nothing in this engine can express "Ctrl+S happened" for a menu item to answer.
    // Displaying the accelerator is what a context menu owes the reader; making it fire belongs to the task
    // that gives the input layer key chords.
    inline entt::entity AddMenuItem( ::Desert::Core::Scene& scene, entt::entity parent, const char* label,
                                     const char* accelerator, bool disabled )
    {
        const entt::entity h   = AddUIChild<ECS::UIButtonComponent>( scene, parent, label );
        auto&              reg = scene.GetRegistry();
        auto&              L   = reg.get<ECS::UILayoutComponent>( h ).Data;
        L.AnchorMin            = { 0.0f, 0.0f };
        L.AnchorMax            = { 1.0f, 0.0f };
        L.OffsetMin            = { 0.0f, 0.0f };
        L.OffsetMax            = { 0.0f, 26.0f };
        auto& B                = reg.get<ECS::UIButtonComponent>( h ).Data;
        B.NormalColor          = { 0.16f, 0.17f, 0.21f };
        B.HoverColor           = { 0.24f, 0.30f, 0.42f };
        B.PressedColor         = { 0.30f, 0.40f, 0.58f };
        B.Disabled             = disabled;
        AddOverlayText( scene, h, "Label", label, { 10.0f, 0.0f }, { 150.0f, 26.0f }, 14.0f );
        if ( accelerator != nullptr && *accelerator != '\0' )
        {
            const entt::entity a = AddOverlayText( scene, h, "Accelerator", accelerator, { 150.0f, 0.0f },
                                                   { 230.0f, 26.0f }, 13.0f );
            auto&              t = reg.get<ECS::UITextComponent2D>( a ).Data;
            t.Align              = ECS::UITextAlign::Right;
            t.Color              = { 0.55f, 0.57f, 0.62f };
        }
        return h;
    }

    // Create an overlay canvas of @p kind, with a working example of its chrome, and return the canvas.
    inline entt::entity CreateUIOverlay( ::Desert::Core::Scene& scene, ECS::UIOverlayKind kind )
    {
        const char* canvasName = "UI Overlay";
        switch ( kind )
        {
            case ECS::UIOverlayKind::Tooltip:
                canvasName = "Tooltip";
                break;
            case ECS::UIOverlayKind::ContextMenu:
                canvasName = "Context Menu";
                break;
            case ECS::UIOverlayKind::Modal:
                canvasName = "Modal";
                break;
            case ECS::UIOverlayKind::Toast:
                canvasName = "Toasts";
                break;
        }

        auto&              e      = scene.CreateNewEntity( std::string( canvasName ) );
        const entt::entity canvas = e.GetHandle();
        e.AddComponent<ECS::UICanvasComponent>();
        e.AddComponent<ECS::UIOverlayComponent>();

        auto& reg = scene.GetRegistry();
        auto& cd  = reg.get<ECS::UICanvasComponent>( canvas ).Data;
        auto& od  = reg.get<ECS::UIOverlayComponent>( canvas ).Data;
        od.Kind   = kind;
        od.Name   = canvasName;

        // WHO IS ON TOP OF WHOM, authored and editable. A tooltip describes whatever is in front, including
        // a dialog, so it is the highest; a menu opened from a dialog must be over it; a toast is the one
        // thing that must never cover what the player is doing, so it sits lowest of the four and still
        // above the HUD. These are defaults, not rules — Sort Order is one field in Details.
        switch ( kind )
        {
            case ECS::UIOverlayKind::Tooltip:
                cd.SortOrder = 400;
                break;
            case ECS::UIOverlayKind::ContextMenu:
                cd.SortOrder = 300;
                break;
            case ECS::UIOverlayKind::Modal:
                cd.SortOrder = 200;
                break;
            case ECS::UIOverlayKind::Toast:
                cd.SortOrder = 100;
                break;
        }

        const auto panel = [&]( entt::entity parent, const char* name, const glm::vec2& anchorMin,
                                const glm::vec2& anchorMax, const glm::vec2& min, const glm::vec2& max )
        {
            const entt::entity h = AddUIChild<ECS::UIPanelComponent>( scene, parent, name );
            auto&              L = reg.get<ECS::UILayoutComponent>( h ).Data;
            L.AnchorMin          = anchorMin;
            L.AnchorMax          = anchorMax;
            L.OffsetMin          = min;
            L.OffsetMax          = max;
            auto& P              = reg.get<ECS::UIPanelComponent>( h ).Data;
            P.Color              = { 0.11f, 0.12f, 0.15f };
            P.CornerRadius       = 6.0f;
            return h;
        };

        switch ( kind )
        {
            case ECS::UIOverlayKind::Tooltip:
            {
                od.FollowPointer = true;
                od.OpenDelay     = 0.4f;
                const entt::entity p =
                     panel( canvas, "Bubble", { 0.0f, 0.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f }, { 220.0f, 40.0f } );
                // The label the runtime fills. The key is the contract between UIOverlay.cpp and this tree;
                // an author who renames it gets an empty bubble, which is why the factory writes it.
                AddOverlayText( scene, p, "Text", "Tooltip", { 10.0f, 0.0f }, { 210.0f, 40.0f }, 14.0f,
                                ::Desert::UI::kOverlayTextKey );
                break;
            }
            case ECS::UIOverlayKind::ContextMenu:
            {
                const entt::entity p =
                     panel( canvas, "Menu", { 0.0f, 0.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f }, { 240.0f, 96.0f } );
                auto& g   = reg.emplace<ECS::UILayoutGroupComponent>( p ).Data;
                g.Type    = ECS::UILayoutType::Vertical;
                g.Padding = glm::vec4( 4.0f );
                g.Spacing = 2.0f;
                reg.get<ECS::UILayoutComponent>( p ).Data.FitHeight = true;
                AddMenuItem( scene, p, "Copy", "Ctrl+C", /*disabled=*/false );
                AddMenuItem( scene, p, "Paste", "Ctrl+V", /*disabled=*/true );
                AddMenuItem( scene, p, "More", "", /*disabled=*/false );
                break;
            }
            case ECS::UIOverlayKind::Modal:
            {
                const entt::entity p = panel( canvas, "Dialog", { 0.5f, 0.5f }, { 0.5f, 0.5f },
                                              { -200.0f, -90.0f }, { 200.0f, 90.0f } );
                AddOverlayText( scene, p, "Title", "Are you sure?", { 20.0f, 16.0f }, { 380.0f, 44.0f }, 18.0f );
                AddOverlayText( scene, p, "Body", "This cannot be undone.", { 20.0f, 52.0f }, { 380.0f, 84.0f },
                                14.0f );
                const entt::entity ok  = AddUIChild<ECS::UIButtonComponent>( scene, p, "Confirm" );
                auto&              okL = reg.get<ECS::UILayoutComponent>( ok ).Data;
                okL.AnchorMin = okL.AnchorMax = { 0.0f, 0.0f };
                okL.OffsetMin                 = { 210.0f, 120.0f };
                okL.OffsetMax                 = { 300.0f, 152.0f };
                const entt::entity cancel     = AddUIChild<ECS::UIButtonComponent>( scene, p, "Cancel" );
                auto&              cL         = reg.get<ECS::UILayoutComponent>( cancel ).Data;
                cL.AnchorMin = cL.AnchorMax = { 0.0f, 0.0f };
                cL.OffsetMin                = { 310.0f, 120.0f };
                cL.OffsetMax                = { 380.0f, 152.0f };
                AddOverlayText( scene, ok, "Label", "Confirm", { 8.0f, 0.0f }, { 82.0f, 32.0f }, 14.0f );
                AddOverlayText( scene, cancel, "Label", "Cancel", { 8.0f, 0.0f }, { 62.0f, 32.0f }, 14.0f );
                break;
            }
            case ECS::UIOverlayKind::Toast:
            {
                const entt::entity stack = AddUIChild<ECS::UIPanelComponent>( scene, canvas, "Stack" );
                auto&              L     = reg.get<ECS::UILayoutComponent>( stack ).Data;
                L.AnchorMin = L.AnchorMax                            = { 1.0f, 1.0f };
                L.OffsetMin                                          = { -320.0f, -220.0f };
                L.OffsetMax                                          = { -20.0f, -20.0f };
                L.HitTest                                            = ECS::UIHitTest::None;
                reg.get<ECS::UIPanelComponent>( stack ).Data.Opacity = 0.0f; // a container, not a surface
                auto& g   = reg.emplace<ECS::UILayoutGroupComponent>( stack ).Data;
                g.Type    = ECS::UILayoutType::Vertical;
                g.Spacing = 8.0f;

                // One slot per authored ToastSlots. A slot that is not filled this frame is COLLAPSED by its
                // Visible binding, so the stack closes up instead of leaving holes — which is the whole
                // reason UIVisibility::Collapsed exists (У4) and the one place in the shipped UI that needs
                // it.
                for ( int i = 0; i < od.ToastSlots; ++i )
                {
                    const entt::entity slot =
                         panel( stack, ( "Slot " + std::to_string( i ) ).c_str(), { 0.0f, 0.0f }, { 0.0f, 0.0f },
                                { 0.0f, 0.0f }, { 300.0f, 52.0f } );
                    auto& sl    = reg.get<ECS::UILayoutComponent>( slot ).Data;
                    sl.HitTest  = ECS::UIHitTest::None;
                    auto& bind  = reg.emplace<ECS::UIBindingComponent>( slot ).Data;
                    bind.Key    = ::Desert::UI::OverlayToastVisibleKey( i );
                    bind.Target = ECS::UIBindTarget::Visible;
                    AddOverlayText( scene, slot, "Text", "", { 12.0f, 0.0f }, { 290.0f, 52.0f }, 14.0f,
                                    ::Desert::UI::OverlayToastTextKey( i ).c_str() );
                }
                break;
            }
        }

        // ONE undo step for the whole overlay: NotifyCreated takes ROOTS, and deleting the canvas takes its
        // subtree with it, so recording the canvas records the chrome.
        return RecordUICreation( scene, canvas );
    }

    // Create catalog entry @p index under @p parent. The switch is generated from the same macro as
    // kUIElements, so an entry can never be listed in a menu without a creator behind it.
    inline entt::entity CreateUIElement( ::Desert::Core::Scene& scene, entt::entity parent, std::size_t index )
    {
        std::size_t  i      = 0;
        entt::entity result = entt::null;

#define DESERT_UI_ELEMENT_CREATE( Type, EntityName, Icon, Label )                                                 \
    if ( index == i++ )                                                                                           \
        return RecordUICreation( scene, AddUIChild<ECS::Type>( scene, parent, EntityName ) );

        DESERT_UI_ELEMENT_LIST( DESERT_UI_ELEMENT_CREATE )

#undef DESERT_UI_ELEMENT_CREATE

        return result;
    }
} // namespace Desert::Editor
