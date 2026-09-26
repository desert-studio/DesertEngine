#pragma once

#include <Engine/ECS/Entity.hpp>
#include <Editor/Panels/PropertyEditor/PropertyEditorBuilder.hpp>
#include <Editor/Widgets/PreviewInput.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct ImGuiTextFilter;
struct ImVec2;

namespace Desert::Core
{
    class Scene;
}
namespace Desert::Assets
{
    class AssetManager;
}
namespace Desert::Animation
{
    class AnimationLibrary;
}
namespace Desert::Editor::UI
{
    class UIHelper;
}

namespace Desert::Editor
{
    class PreviewViewport;

    // Runtime services handed to a component's Draw callback (built once per frame by ComponentEditor).
    struct ComponentEditContext
    {
        std::weak_ptr<Assets::AssetManager> AssetManager;
        const Animation::AnimationLibrary*  AnimationLibrary = nullptr;
        UI::UIHelper*                       UIHelper         = nullptr;

        // The Details panel's live preview renderer, lent to whichever component wants a thumbnail of what
        // the entity renders (the 3D Model row uses it). A component only DRAWS it — the panel owns it and
        // records its offscreen render in OnPreUpdate, because rendering from inside the ImGui pass
        // destroys descriptor pools bound to the recording command buffer. Setting PreviewUsed tells the
        // panel to pay for next frame's render; leaving it alone stops the GPU work.
        PreviewViewport* Preview     = nullptr;
        UI::UIHelper*    PreviewUI   = nullptr;
        bool*            PreviewUsed = nullptr;

        // Draws the shared preview at @p size and marks it as used. Returns false when the panel did not
        // lend one, so a component never has to know where it came from — the caller falls back to a
        // cached thumbnail.
        //
        // The mode comes from @p kind (DetailsPreviewInteraction, Editor/Widgets/PreviewInput.hpp): the
        // Static Mesh row orbits and zooms, the Skybox row keeps one angle. A Static row's double-click opens
        // @p openHandle — the asset the row stands for — through the same AssetFieldRequests queue every
        // other asset field in Details uses; an Interactive row's double-click re-frames instead.
        [[nodiscard]] bool DrawPreview( const ImVec2& size, DetailsPreviewKind kind, uint64_t openHandle ) const;

        // Details search box: while non-empty, reflected components draw only the fields that match.
        // A hand-written widget cannot filter itself — the panel decides whether to draw it at all.
        const char* FieldFilter = nullptr;

        // MAY THIS WIDGET OFFER A JUMP TO ANOTHER PANEL?
        //
        // A component's editor is no longer drawn only by Details. The Clouds window draws the cloud
        // layer's own entry as its first stage — deliberately the same code, so the two windows cannot come
        // to disagree about what a layer has in it — and the cloud entry offers a button that opens the
        // Clouds window. Inside the Clouds window that button takes the user where they already are, which
        // is a control that does nothing (§1.3).
        //
        // TRUE BY DEFAULT, because Details is the host every widget was written for and a new host that
        // forgets to say is one that shows a jump rather than one that hides a control.
        bool AllowPanelJumps = true;

        Assets::AssetManager* AssetMgr() const
        {
            return AssetManager.lock().get();
        }
    };

    // One registered component editor. Built by the helpers/macros below and consumed by ComponentEditor.
    struct ComponentEditorEntry
    {
        std::string Name;
        bool        CanRemove = true;

        std::function<bool( ECS::Entity& )>                                                      Has;
        std::function<void( ECS::Entity& )>                                                      Add;
        std::function<void( ECS::Entity& )>                                                      Remove;
        std::function<void( ECS::Entity&, ::Desert::Core::Scene*, const ComponentEditContext& )> Draw;

        // REFLECTED components only (empty/null for hand-written widgets): the reflected data block and
        // its type name. They let the panel read the component WITHOUT drawing it — for the collapsed
        // header summary, the search filter and pinned fields. A custom widget opts out of all three by
        // simply not having them.
        std::string                          ReflectedTypeName;
        std::function<void*( ECS::Entity& )> DataPtr;
    };

    // The registered name of the cloud layer's component editor. Stated ONCE because it now has two
    // readers — the registration in ComponentEditorRegistrations.cpp and the Clouds window's first stage —
    // and a name that agreed only by inspection would fail as a blank pane rather than as an error.
    inline constexpr const char* kVolumetricCloudComponentEditor = "Volumetric Cloud";

    // Editor-side registry of component editors. Components self-register at static-init via the macros
    // below, so adding one never touches ComponentEditor / the Details panel.
    class ComponentWidgetRegistry
    {
    public:
        static ComponentWidgetRegistry& Get();

        // Returns a dummy int so the call can seed a static variable (self-registration).
        int                                      Register( ComponentEditorEntry entry );
        const std::vector<ComponentEditorEntry>& Entries() const
        {
            return m_Entries;
        }

        // ONE COMPONENT'S EDITOR, BY NAME — for a panel other than Details that has to draw exactly one
        // component. The Clouds window's first stage IS the cloud layer's component, and drawing it any
        // other way would be a second copy of fields this registry already knows how to draw.
        //
        // NULL WHEN NOTHING IS REGISTERED UNDER THAT NAME, so the caller can say so rather than drawing an
        // empty pane. The name is a registry key and the constants below are the only ones any caller
        // outside the registration file should spell.
        [[nodiscard]] const ComponentEditorEntry* Find( std::string_view name ) const;

    private:
        std::vector<ComponentEditorEntry> m_Entries;
    };

    // Collects the field-pointers of the SAME reflected data block on every OTHER selected entity that
    // has the component. @p fieldPtrOrNull maps an entity to its field pointer (or nullptr when the
    // entity lacks the component). Defined in ComponentMultiEdit.cpp so this header stays light.
    std::vector<void*>
    GatherSelectionFieldPtrs( ::Desert::Core::Scene* scene, const ECS::Entity& primary,
                              const std::function<void*( const ECS::Entity& )>& fieldPtrOrNull );

    // Renders the search box + the grouped (category submenus) / filtered-flat list of components addable to
    // @p entity, adding the chosen one (undoable). SINGLE source of truth for the categorization — reused by
    // the Details "Add Component" popup AND the scene-outliner context menu, so the buckets live in one place.
    void DrawAddComponentMenu( ECS::Entity& entity, ImGuiTextFilter& filter );

    // Reflected component: ZERO UI code — the panel is auto-built from the data block's REFLECT() metadata.
    // When several entities are selected, edits broadcast to all of them (multi-select Details).
    template <class ComponentT, class DataT>
    ComponentEditorEntry MakeReflectedComponentEntry( std::string name, std::string dataTypeName,
                                                      DataT ComponentT::*member, bool canRemove = true )
    {
        ComponentEditorEntry e;
        e.Name              = std::move( name );
        e.CanRemove         = canRemove;
        e.ReflectedTypeName = dataTypeName;
        e.Has               = []( ECS::Entity& en ) { return en.HasComponent<ComponentT>(); };
        e.Add               = []( ECS::Entity& en ) { en.AddComponent<ComponentT>(); };
        e.Remove            = []( ECS::Entity& en ) { en.RemoveComponent<ComponentT>(); };
        e.DataPtr = [member]( ECS::Entity& en ) -> void* { return &( en.GetComponent<ComponentT>().*member ); };
        e.Draw    = [member, dataTypeName]( ECS::Entity& en, ::Desert::Core::Scene* scene,
                                         const ComponentEditContext& ctx )
        {
            auto& comp = en.GetComponent<ComponentT>();

            const std::vector<void*> siblings =
                 GatherSelectionFieldPtrs( scene, en,
                                           [member]( const ECS::Entity& other ) -> void*
                                           {
                                               if ( !other.HasComponent<ComponentT>() )
                                                   return nullptr;
                                               return &( other.GetComponent<ComponentT>().*member );
                                           } );

            PropertyEditorBuilder::DrawMulti( &( comp.*member ), siblings, dataTypeName, ctx.AssetMgr(),
                                              ctx.UIHelper, ctx.FieldFilter );
        };
        return e;
    }

    // Custom component: bespoke UI (asset pickers, vector controls, ...). The draw lambda gets the context.
    template <class ComponentT>
    ComponentEditorEntry MakeCustomComponentEntry(
         std::string                                                                              name,
         std::function<void( ECS::Entity&, ::Desert::Core::Scene*, const ComponentEditContext& )> draw,
         bool canRemove = true )
    {
        ComponentEditorEntry e;
        e.Name      = std::move( name );
        e.CanRemove = canRemove;
        e.Has       = []( ECS::Entity& en ) { return en.HasComponent<ComponentT>(); };
        e.Add       = []( ECS::Entity& en ) { en.AddComponent<ComponentT>(); };
        e.Remove    = []( ECS::Entity& en ) { en.RemoveComponent<ComponentT>(); };
        e.Draw      = std::move( draw );
        return e;
    }
} // namespace Desert::Editor

#define DESERT_COMPONENT_CONCAT_( a, b ) a##b
#define DESERT_COMPONENT_CONCAT( a, b ) DESERT_COMPONENT_CONCAT_( a, b )

// Reflected component: one line, no widget class. Auto UI from `DataTypeName`'s reflection metadata.
//   DESERT_REGISTER_REFLECTED_COMPONENT( ECS::FooComponent, Data, "FooData", "Foo" )
#define DESERT_REGISTER_REFLECTED_COMPONENT( ComponentT, Member, DataTypeName, DisplayName )                      \
    namespace                                                                                                     \
    {                                                                                                             \
        const int DESERT_COMPONENT_CONCAT( _desert_component_reg_, __COUNTER__ ) =                                \
             ::Desert::Editor::ComponentWidgetRegistry::Get().Register(                                           \
                  ::Desert::Editor::MakeReflectedComponentEntry<ComponentT>( DisplayName, DataTypeName,           \
                                                                             &ComponentT::Member ) );             \
    }

// Custom component: provide a draw lambda ( ECS::Entity&, Core::Scene*, const ComponentEditContext& ).
// Wrap the lambda in parentheses so its commas don't split the macro arguments.
#define DESERT_REGISTER_CUSTOM_COMPONENT( ComponentT, DisplayName, CanRemove, DrawLambda )                        \
    namespace                                                                                                     \
    {                                                                                                             \
        const int DESERT_COMPONENT_CONCAT( _desert_component_reg_, __COUNTER__ ) =                                \
             ::Desert::Editor::ComponentWidgetRegistry::Get().Register(                                           \
                  ::Desert::Editor::MakeCustomComponentEntry<ComponentT>( DisplayName, DrawLambda, CanRemove ) ); \
    }
