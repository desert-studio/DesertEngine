#pragma once

#include "../IPanel.hpp"
#include "CloudStages.hpp"

#include <memory>
#include <string>

namespace Desert::Assets
{
    class AssetManager;
}
namespace Desert::Core
{
    class Scene;
}
namespace Desert::Editor::UI
{
    class UIHelper;
}

namespace Desert::Editor
{
    class OpenDocuments;

    /**
     * @brief ONE WINDOW FOR THE WHOLE SKY: a rail of the six stages in build order, and the selected
     *        stage's own editor embedded beside it.
     *
     * WHY. Task O7 went looking for a defect in the noise panel and found a wider one: clouds are authored
     * in SIX places and not one of them mentions the other five, so an artist who wants to know where the
     * two layout pictures live has to already know. The owner picked variant B from the drawn sheets. It is
     * also Unreal's own move — their Environment Light Mixer (SEnvironmentLightingViewer.cpp) gathers Sky
     * Light, Directional Light, Sky Atmosphere, Volumetric Cloud and Height Fog into one window as embedded
     * detail views, with "create if missing" buttons where a thing is absent.
     *
     * THE ONE THING THIS WINDOW MUST NOT DO IS OWN A DOCUMENT. `DocumentWell` shows the same documents,
     * and a `.demat` with two working copies is the defect Desert/Tests/Editor/MaterialEditStates exists to
     * prevent — one Apply from a mesh in the level being drawn with the preview's material. So the single
     * instance belongs to Editor/Core/OpenDocuments.hpp and this window is one VIEW over it, equal to the
     * well and unaware of it: it asks the owner for the document that edits the stage's subject, gets THE
     * SAME OBJECT the well has, and calls its OnUIRender in its own pane.
     *
     * DRAWING ONE DOCUMENT IN TWO PLACES IN ONE FRAME WAS MEASURED BEFORE IT WAS RELIED ON, because per
     * (frame x renderer slot) state has bitten this tree before (Docs/RENDERER_FRAME_STATE.md) and a
     * document with a preview is the first candidate. It does not break, and the mechanism is why: a
     * document's GPU work is recorded in OnPreUpdate — which EditorLayer runs ONCE per document per frame,
     * for every open document, whoever draws it — while OnUIRender only submits ImGui commands, and ImGui
     * scopes widget ids per window. Measured on M_Clouds_Protocol_Clouds drawn in two windows at once: both
     * panes showed the same preview and the same values, an edit sent to the document appeared in both in
     * the same frame, and the renderer-slot census stayed at one document holding one slot.
     *
     * WHAT IT DOES NOT DRAW, and the reason rather than the omission. The drawn sheet had a live "the sky
     * right now" pane beside the rail. It is not here: it would be a seventh consumer of the six renderer
     * slots, held for the whole session by a TOOL panel, and it would show the picture stage 2 already
     * shows — the Material Editor's own sky dome, embedded, is that pane whenever the material stage is
     * selected. A slot spent on a second copy of an existing picture is a cost with no information in it.
     *
     * NOTHING IS BOUND HERE. The rail shows what the chain says; a slot is bound in the material's own
     * Inputs table, which is stage 2 of this same window. A second picker here would be a second execution
     * path over one value — see CloudStages.hpp.
     */
    class CloudsPanel final : public IPanel
    {
    public:
        /// The registered panel name, and therefore what Core::PanelRequests::Open addresses. One constant
        /// because two places send the user here (the View menu's own loop, and the Details panel's
        /// buttons) and a typo in either is a button that silently does nothing.
        static constexpr const char* kPanelName = "Clouds";

        CloudsPanel( const std::shared_ptr<Desert::Core::Scene>&  scene,
                     const std::shared_ptr<Assets::AssetManager>& assets, const OpenDocuments& documents );
        ~CloudsPanel() override;

        void OnUIRender() override;
        void SetScene( const std::shared_ptr<Desert::Core::Scene>& scene ) override;

        glm::vec2 GetDefaultSize() const override
        {
            return { 1180.0f, 780.0f };
        }

        // ── DETAILS -> HERE ────────────────────────────────────────────────────────────────────────────
        //
        // The owner's request, verbatim: "свести с панелью Details, чтобы можно было перейти в редактор
        // удобно". Details holds the entity and can reach exactly two of the six stages — the component
        // header is stage 1 and the Material slot is stage 2 — because since O1 the layout, the four types
        // and the noise volume are MATERIAL parameters and the component names none of them. That is a
        // property of the split, not a shortfall, and it is also the argument for stage 2 being the hub.
        //
        // ONE CALL RATHER THAN TWO, and that is not sugar: showing the window and choosing the stage are a
        // single intent, and a caller that did them separately would be able to do half of it. It is the
        // shape Core::PanelRequests already has (a static inbox the panel loop drains) rather than a
        // fourth private wire — see the note at the top of Editor/Core/SubjectOpenRequest.hpp about what
        // happens when every feature invents its own.
        static void OpenAt( CloudStage stage );

    private:
        // Reads the scene and the material chain into a CloudChain. Once per frame, and only READ
        // afterwards: the rail, the header and the embedded pane must all describe the same sky.
        [[nodiscard]] CloudChain GatherChain() const;

        void DrawHeader( const CloudChain& chain );
        void DrawRail( const CloudChain& chain );
        void DrawChainSummary( const CloudChain& chain );
        void DrawStagePane( const CloudChain& chain );
        void DrawLayerStage( const CloudChain& chain );
        void DrawEmbeddedDocument( const CloudChain& chain, const SubjectId& subject );
        void DrawEmptyStage( const CloudChain& chain );

        /// The name to show for an asset handle: its file stem, or a stated reason there is none.
        [[nodiscard]] std::string AssetLabel( const Common::AssetHandle& handle ) const;

        std::shared_ptr<Desert::Core::Scene> m_Scene;
        // The shared_ptr and not the raw pointer, because the component editor this window lends a context
        // to takes a weak_ptr (ComponentEditContext::AssetManager) — the same one Details hands it.
        std::shared_ptr<Assets::AssetManager> m_Assets;
        /// The owner of the open documents. NEVER the owner OF them — this window holds a reference and
        /// asks; see the class note.
        const OpenDocuments* m_Documents = nullptr;

        /// This window's own texture-id cache, for the component editor it lends a context to (stage 1).
        /// Its own rather than shared with Details, because a UIHelper's descriptor sets belong to the
        /// panel that draws with them and must die with it.
        std::unique_ptr<UI::UIHelper> m_UI;

        CloudStage m_Stage = CloudStage::Layer;

        uint32_t m_TypeSlot = 0;
        uint32_t m_Hero     = 0;

        /// The subject this window last ASKED to be opened, and how many frames ago. Requesting every
        /// frame would re-raise the renderer-slot refusal dialog for ever on a sky with more stages than
        /// slots; requesting once and saying what happened is the honest version.
        SubjectId m_Requested;
        uint32_t  m_FramesSinceRequest = 0;

        /// The subject this pane HAS BEEN SHOWING. Its whole job is to make a close mean a close.
        ///
        /// Without it the window re-opens whatever it is pointed at the moment the document goes away —
        /// so closing the material's tab in the document well, or running Close from the palette, put it
        /// straight back and the close read as a button that does nothing. MEASURED, not imagined: the
        /// close was logged and the document was open again in the same reply.
        ///
        /// With it, a document that WAS here and is not any more is a thing the user did, and the pane
        /// says so and offers to open it again rather than deciding for them. Cleared whenever the stage
        /// or the selection inside it moves, because that is a new question and the answer to the old one
        /// no longer applies.
        SubjectId m_Showed;
    };
} // namespace Desert::Editor
