#pragma once

#include <Engine/Desert.hpp>

#include "../IPanel.hpp"

#include "ComponentEditor.hpp"

#include <Editor/Widgets/ThumbnailCache.hpp>
#include <Editor/Widgets/UIHelper/ImGuiUI.hpp>

#include <Common/Core/Constants.hpp>

namespace Desert::Editor
{
    class ScenePropertiesPanel final : public IPanel
    {
    public:
        explicit ScenePropertiesPanel( const std::shared_ptr<Desert::Core::Scene>&  scene,
                                       const std::shared_ptr<Assets::AssetManager>& assetManager,
                                       const Animation::AnimationLibrary*           animationLibrary )
             : IPanel( "Details" ), m_Scene( scene ), m_AssetManager( assetManager ),
               m_AnimationLibrary( animationLibrary )
        {
        }
        void OnUIRender() override;
        void SetScene( const std::shared_ptr<Desert::Core::Scene>& scene ) override
        {
            m_Scene = scene;
        }

    private:
        // UE-style property search, drawn above the scrolling component list.
        void DrawSearchBox();

        // What the panel draws instead of its body when nothing is selected: names itself and offers the
        // one action that ends the state (create an entity and select it).
        void DrawNoSelectionState();

    private:
        std::shared_ptr<Desert::Core::Scene>        m_Scene;
        const std::shared_ptr<Assets::AssetManager> m_AssetManager;
        const Animation::AnimationLibrary*          m_AnimationLibrary;
        bool                                        m_DebugMode = false;
        std::string m_PrefabSavePath = Common::Constants::Path::PREFAB_PATH.string(); // post-remap

        // Details search text; empty = show everything. Owned by the panel (one search per Details view).
        std::string m_FieldSearch;
        // The component list renderer. A member, not a function-static: several Details panels can exist
        // (one per scene view) and they must not share one editor's state.
        std::unique_ptr<ComponentEditor> m_ComponentEditor;

        // --- Asset pictures ---------------------------------------------------------------------------
        // NO LIVE VIEW IN DETAILS (THM-FIXF). UE's Details slots show thumbnails from the shared pool — the
        // Content Browser's pictures — and a live render view exists only inside an asset window, dying with
        // it. This panel used to own a full SceneRenderer (its own shadow cascade, SMAA, fog) the moment a
        // mesh was clicked, and kept it rendering while the entity stayed selected: 140 -> 100 FPS measured.
        // What is left is the texture-id cache the rows blit ThumbnailService's PNGs through.
        std::unique_ptr<UI::UIHelper> m_ThumbnailUI;
    };
} // namespace Desert::Editor