#pragma once

#include <Engine/Desert.hpp>

#include "../IPanel.hpp"

#include "ComponentEditor.hpp"

#include <Editor/Widgets/PreviewViewport.hpp>
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
        // Where the asset preview is rendered — offscreen work must never happen inside the ImGui pass.
        void OnPreUpdate() override;
        void SetScene( const std::shared_ptr<Desert::Core::Scene>& scene ) override
        {
            m_Scene = scene;
        }

        // Is the Details preview holding one of the six renderer slots right now? Asked by the editor's slot
        // census before a new asset-document window is admitted — Details is the largest demand-driven
        // consumer, and a census that guessed at it would name the wrong thing to close.
        [[nodiscard]] bool HoldsRendererSlot() const
        {
            return m_Preview != nullptr;
        }

    private:
        // UE-style property search, drawn above the scrolling component list.
        void DrawSearchBox();

        // What the panel draws instead of its body when nothing is selected: names itself and offers the
        // one action that ends the state (create an entity and select it).
        void DrawNoSelectionState();

        /**
         * @brief Build the viewport if there is a renderer slot to give it. FALSE means there is not.
         *
         * IT ANSWERS, and the answer is not decoration. Every line after the call used to dereference
         * m_Preview unconditionally, correctly, because this function could not fail. Teaching it to
         * decline when all six slots are taken (Engine/Core/RendererSlotBudget.hpp) put a null back into
         * a place three callers assumed could not hold one — and the Update() at the end of OnPreUpdate is
         * driven by a flag raised on the PREVIOUS UI frame, so it outlives the renderer by exactly one
         * frame. Measured: five open material documents plus one click on a mesh entity killed the editor
         * on the frame after the refusal was logged. A bool nobody can ignore is the difference.
         *
         * The renderer, and with it the slot, are still claimed lazily on the first Update().
         */
        [[nodiscard]] bool EnsurePreview();
        void ReleasePreview(); // destroy it, which is what returns the slot

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

        // --- Asset preview -------------------------------------------------------------------------
        // ONE renderer for the whole panel (a SceneRenderer is not cheap), lent to whichever component
        // wants a live thumbnail of what the entity renders — today the 3D Model row. The panel keeps the
        // frame ordering (Update in OnPreUpdate); a component only blits the last image.
        //
        // A second live renderer used to corrupt the viewport, because per-frame scene state lived in the
        // shared material. It no longer does: every such resource is stored per (frame x renderer slot),
        // and this panel's renderer holds its own slot (Docs/RENDERER_FRAME_STATE.md, shape B).
        //
        // Null whenever there is nothing to preview — no selection, or the panel closed. There are only
        // six slots, and this used to be a VALUE member: once a single selection had shown a thumbnail it
        // held one of the six until the editor quit, even with the panel closed. Destroying it is the
        // mechanism, not an optimisation on top of one — a viewport that is merely told to stop drawing
        // still owns its SceneRenderer. Same shape as MaterialPreviewPanel::m_Preview, deliberately.
        std::unique_ptr<PreviewViewport> m_Preview;
        std::unique_ptr<UI::UIHelper> m_ThumbnailUI;           // texture ids for the preview image + cached PNGs
        uint64_t                      m_PreviewKey    = 0;     // what it shows; a change re-points and re-frames
        bool                          m_PreviewActive = false; // a component drew it during the last UI frame
        // Already said out loud that there was no slot to build the preview in. Latched so the warning is
        // one line per stretch of scarcity rather than one per frame, and cleared the moment a slot frees
        // up — a state the user leaves by closing a window has to be able to be reported again.
        bool m_PreviewSlotRefused = false;
    };
} // namespace Desert::Editor