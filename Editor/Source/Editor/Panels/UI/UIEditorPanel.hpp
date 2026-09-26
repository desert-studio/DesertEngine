#pragma once

#include "../IPanel.hpp"

#include <Editor/Widgets/UIHelper/ImGuiUI.hpp>

#include <Common/Core/UUID.hpp>

#include <Engine/Graphic/Render2D/Render2D.hpp>
#include <Engine/UI/UICanvasContext.hpp>

#include <entt/entt.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace Desert::Core
{
    class Scene;
}

namespace Desert::ECS
{
    struct UICanvasComponent;
}

namespace Desert::Graphic
{
    class Framebuffer;
    class RenderPass;
} // namespace Desert::Graphic

namespace Desert::Editor
{
    // ── ONE CANVAS: A DOCUMENT, NOT A TOOL ────────────────────────────────────────────────────────────
    //
    // Authoring surface for one screen-space UI canvas: a toolbar that places elements on it and a preview
    // of it drawn by the shipping renderer.
    //
    // THE SUBJECT IS A COMPONENT, NOT A FILE. There is no UI-canvas asset; the canvas lives in
    // ECS::UICanvasComponent and is serialized with the scene — the case Editor/Core/EditorSubject.hpp was
    // written for, and the reason this window could not be a document before U7.
    //
    // IT USED TO EDIT *THE FIRST CANVAS IT FOUND*. The panel asked FindUICanvas(registry) for the scene's
    // canvas, which answers with `*view.begin()` — so a scene with two canvases had one that this window
    // could never be pointed at, and no control anywhere to choose. That is not a missing feature, it is
    // what a singleton window over a per-entity component has to do: with one window there is nothing to
    // say WHICH. A subject says it, and two canvases are two windows.
    //
    // AND THE ENGINE DREW ONLY THE FIRST ONE, which U7-2 named as the half it did not fix and refused the
    // preview over rather than showing the wrong canvas. Ю1 closed it: UI::RenderCanvas2D takes the canvas
    // as an argument, so this window hands it the entity its own subject names and the refusal is gone. Two
    // canvases are two windows, and both of them draw.
    //
    // AND ITS "CREATE UI CANVAS" EMPTY STATE IS GONE WITH IT. A document is opened OVER a canvas that
    // exists; making one is Details ▸ Add Component ▸ UI Canvas and the viewport toolbar's UI ▸ UI Canvas
    // (ViewportPanel.cpp), both of which were already there. An empty state in a document is a window about
    // nothing, which is the shape U7 removed from the anim graph and the particle editor.
    //
    // THE PREVIEW IS DRAWN BY THE SHIPPING RENDERER. It used to be drawn by a second, ImGui-based canvas
    // renderer that knew six of the engine's twenty-two UI component types, so pressing "+ Slider" in this
    // very panel added a slider its own preview could not show. That renderer is gone; this window runs
    // UI::RenderCanvas2D — the function the game and the viewport run — into its own offscreen target and
    // shows the result as an image.
    //
    // FRAME ORDERING IS PART OF THE CONTRACT, as in PreviewViewport: RenderPreview() records a render pass
    // and must run from OnPreUpdate(); OnUIRender() only shows the image it produced. Recording from inside
    // the ImGui pass would release pipelines and descriptor pools bound to the recording command buffer.
    class UIEditorPanel final : public ISubjectDocument
    {
    public:
        // The subject type this editor is registered under — one literal, read by the registration and by
        // the Details button, because two that must agree is the shape that drifts.
        static constexpr const char* kComponentTypeName = "UICanvasComponent";

        [[nodiscard]] static SubjectTypeKey SubjectType()
        {
            return ComponentSubjectType( kComponentTypeName );
        }

        // The subject for ONE entity's canvas — what a Details button sends.
        [[nodiscard]] static SubjectId SubjectFor( const Common::UUID& entity )
        {
            return ComponentSubject( entity, kComponentTypeName );
        }

        UIEditorPanel( const SubjectId& subject, const std::string& displayName,
                       const std::shared_ptr<::Desert::Core::Scene>& scene );

        // Waits for device idle before releasing the preview target: it owns a framebuffer and the three
        // Render2D pipelines built against it, which a submitted frame may still be executing.
        ~UIEditorPanel() override;

        [[nodiscard]] glm::vec2 GetDefaultSize() const override
        {
            return { 900.0f, 560.0f };
        }

        // Records this frame's canvas render into the offscreen target (see the class comment).
        void OnPreUpdate() override;
        void OnUIRender() override;

        // The entity, in the scene this document was opened over, still carrying a UICanvasComponent.
        // Deleting the entity, removing the component, or closing the scene are three ways for this
        // window's subject to stop existing and the user experiences them as one.
        [[nodiscard]] bool IsSubjectAlive() const override
        {
            return ResolveCanvas() != nullptr;
        }

        // ── NOT A VIEW, EVEN THOUGH THIS WINDOW RENDERS ─────────────────────────────────────────────────
        //
        // A view is a live Graphic::SceneRenderer (counted by ViewResourceRegistry::LiveCount). This window
        // has none: it owns a Framebuffer, a RenderPass and a Render2D, and draws the canvas straight into
        // them. So it can never claim a view and can never release one, and BOTH answers have to be false rather
        // than the base class's conservative default — a `true` here would put "UICanvasComponent document 'HUD' —
        // holds a slot" in the refusal census (EditorLayer::RendererSlotCensus), which tells a user to close a
        // window that frees nothing. Its own GPU resources are released by the undrawn sweep below and by ~this.
        [[nodiscard]] bool HoldsView() const override
        {
            return false;
        }

        [[nodiscard]] bool ClaimsView() const override
        {
            return false;
        }

        // DELIBERATELY A NO-OP, as in AnimGraphPanel and ParticleEditorPanel, which say why at length: the
        // subject is an entity UUID and UUIDs belong to one registry, so following the active-scene fanout
        // would point this window at a scene where the id names nothing — or names somebody else's entity.
        void SetScene( const std::shared_ptr<Desert::Core::Scene>& /*scene*/ ) override
        {
        }

    private:
        // ── GIVE THE PICTURE BACK WHILE NOBODY IS LOOKING ──────────────────────────────────────────────
        //
        // A document is open for as long as the user keeps it, which is not the same as on screen: a tab
        // behind another tab draws nothing and ImGui::Begin says so (EditorLayer::DrawDocumentWell skips
        // OnUIRender for it). Without this, a hidden UI Editor tab would record a full canvas pass into a
        // design-resolution RGBA32F target every frame for a window nobody can see — the exact waste
        // ISubjectDocument::ReleaseView exists to stop.
        //
        // ITS OWN COUNT AND NOT THE EDITOR'S. ReleaseSlotsOfHiddenDocuments is gated on
        // HoldsView(), which this document answers false for the reason above, so the six-slot
        // path cannot serve it. The two numbers are independent — nothing has to keep them equal — and
        // they are the same order of magnitude because the question is the same one: has the user left?
        static constexpr uint32_t kFramesUndrawnBeforeTargetRelease = 30;

        // The canvas this window edits, or nullptr when it is gone. ONE resolution, used by the draw, by
        // the preview and by the liveness answer, so the three cannot disagree.
        [[nodiscard]] ECS::UICanvasComponent* ResolveCanvas() const;

        // The entity handle behind the same subject, for the element factory (which parents by handle).
        // entt::null when the subject is dead — asked only where ResolveCanvas() already answered.
        [[nodiscard]] entt::entity ResolveCanvasEntity() const;

        // (Re)create the offscreen target + the Render2D pipelines for a @p width x @p height design
        // resolution. Returns false and fills m_PreviewError (logged with the reason) on failure.
        bool EnsureTarget( uint32_t width, uint32_t height );

        // Release the target and everything built against it, behind a device-idle wait.
        void ReleaseTarget();

        // WEAK, not shared — see AnimGraphPanel for the argument. A closed scene is one of the ways this
        // document's subject dies, and a strong reference would hide that and leak the level with it.
        std::weak_ptr<::Desert::Core::Scene> m_Scene;

        std::unique_ptr<Editor::UI::UIHelper> m_UIHelper;
        std::shared_ptr<Graphic::Framebuffer> m_Target;
        std::shared_ptr<Graphic::RenderPass>  m_RenderPass;
        Graphic::Render2D::Render2D           m_Render2D;

        // This preview's own canvas state, so its walk cannot touch the viewport's. DrivesSceneAnimation is
        // false because a UIAnim clip's playhead lives in the component (the Sequencer scrubs it) and is
        // therefore SCENE state: the viewport pass advances it, and a second view advancing it too would run
        // every clip at twice its authored speed.
        ::Desert::UI::UIViewContext m_UIView;

        uint32_t m_TargetWidth  = 0;
        uint32_t m_TargetHeight = 0;

        // True once OnPreUpdate has recorded a canvas into m_Target this frame — OnUIRender only shows the
        // image when it has one, so a window opened mid-frame draws its chrome and the picture one frame on.
        bool m_PreviewRecorded = false;

        // Set by OnUIRender, read and cleared by the NEXT frame's OnPreUpdate — which is the order the
        // editor runs them in (every OnPreUpdate, then the document well). So this is "was I on screen last
        // frame", which is the only honest thing a document can ask on its own.
        bool     m_DrawnLastFrame = false;
        uint32_t m_FramesUndrawn  = 0;

        // Non-empty when the target or its pipelines could not be built. Shown in the window AND logged at
        // the site with the reason; a preview that silently stays black is the fallback this project bans.
        std::string m_PreviewError;
    };
} // namespace Desert::Editor
