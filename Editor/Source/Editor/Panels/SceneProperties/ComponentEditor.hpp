#pragma once

#include <Engine/Desert.hpp>
#include <ImGui/imgui_internal.h>

#include <Editor/Panels/PropertyEditor/ComponentWidgetRegistry.hpp>

namespace Desert::Editor
{
    // Renders the Details panel for an entity by iterating the ComponentWidgetRegistry — components
    // self-register at static-init (see ComponentWidgetRegistry.hpp), so adding one never touches this
    // class. Reflected components get an auto-built UI; asset-bearing ones supply a custom draw lambda.
    class ComponentEditor
    {
    public:
        ComponentEditor( const std::shared_ptr<Assets::AssetManager>& assetManager,
                         const Animation::AnimationLibrary*           animationLibrary );

        // @p fieldFilter is the Details search box (null/empty = show everything). Reflected components
        // drop the fields that don't match; a hand-written widget can only be matched on its NAME, so it
        // is either drawn whole or not at all.
        void Render( ECS::Entity& entity, ::Desert::Core::Scene* scene = nullptr,
                     const char* fieldFilter = nullptr );

        // The texture-id cache the rows blit ThumbnailService's pictures through. Details lends no live view
        // (THM-FIXF): an asset row is a thumbnail, as in UE.
        void SetThumbnailUI( UI::UIHelper* ui )
        {
            m_ThumbnailUI = ui;
        }

    private:
        ComponentEditContext MakeContext() const;
        void                 RenderAddComponentPopup( ECS::Entity& entity );
        void RenderComponentHeader( const ComponentEditorEntry& entry, ECS::Entity& entity,
                                    ::Desert::Core::Scene* scene, const ComponentEditContext& ctx );

        // The "Pinned" section above every component: fields the user starred, hoisted out of their
        // components so the values they actually tune are always in reach. Reflected fields only.
        void DrawPinnedFields( ECS::Entity& entity, const ComponentEditContext& ctx );

        // Is this component worth showing for the current search? Matches its name, or (reflected only)
        // any of its field labels.
        static bool EntryMatchesFilter( const ComponentEditorEntry& entry, const char* filter );

    private:
        UI::UIHelper* m_ThumbnailUI = nullptr;

        std::weak_ptr<Assets::AssetManager> m_AssetManager;
        const Animation::AnimationLibrary*  m_AnimationLibrary = nullptr;
        ImGuiTextFilter                     m_ComponentFilter;
    };

} // namespace Desert::Editor
