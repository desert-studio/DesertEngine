#pragma once

#include "../IPanel.hpp"

#include <Common/Core/UUID.hpp>

#include <Engine/Assets/Common.hpp>

#include <memory>
#include <string>
#include <vector>

namespace ax::NodeEditor
{
    struct EditorContext;
}
namespace Desert::Core
{
    class Scene;
}
namespace Desert::Animation
{
    class AnimationLibrary;
}
namespace Desert::ECS
{
    struct AnimationComponent;
}
namespace Desert::Assets
{
    class AssetManager;
    class AnimGraphAsset;
} // namespace Desert::Assets

namespace Desert::Editor
{
    // ── THE ANIM GRAPH OF ONE ENTITY: A DOCUMENT, NOT A TOOL ──────────────────────────────────────────
    //
    // An imgui-node-editor canvas over ONE AnimationComponent's graph — STATES are nodes, TRANSITIONS are
    // links. A side panel edits the selected state (clip/loop/speed/entry) or transition
    // (blend/exit-time/conditions), plus parameters with live value controls.
    //
    // IT USED TO BE A SINGLETON THAT FOLLOWED THE SELECTION, and that is what changed. There was one Anim
    // Graph window; it drew whatever entity happened to be selected, so clicking a crate in the viewport
    // emptied it, two characters could not be compared side by side, and the panel's own header had to
    // carry "Select an animated entity" as a state. The subject is now fixed at construction and the
    // window is about that entity for as long as it exists — which is what the owner asked for when he
    // said these tabs should open FROM the thing rather than sit in a menu.
    //
    // THE SUBJECT IS A COMPONENT, NOT A FILE. There is no anim-graph asset: the graph lives in
    // ECS::AnimationComponent and is serialized with the scene. That is exactly the case the old seam
    // could not express, and Editor/Core/EditorSubject.hpp is the answer to it.
    class AnimGraphPanel final : public ISubjectDocument
    {
    public:
        // The subject type this editor is registered under. Named here rather than spelled at the
        // registration and at the Details button separately: two literals that must agree is the shape
        // that drifts, and a component whose facet is written out twice is a button that opens nothing.
        static constexpr const char* kComponentTypeName = "AnimationComponent";

        [[nodiscard]] static SubjectTypeKey SubjectType()
        {
            return ComponentSubjectType( kComponentTypeName );
        }

        // The subject for ONE entity's anim graph — what a Details button sends.
        [[nodiscard]] static SubjectId SubjectFor( const Common::UUID& entity )
        {
            return ComponentSubject( entity, kComponentTypeName );
        }

        AnimGraphPanel( const SubjectId& subject, const std::string& displayName,
                        const std::shared_ptr<::Desert::Core::Scene>& scene,
                        const Animation::AnimationLibrary* library, Assets::AssetManager* assetManager );
        ~AnimGraphPanel() override;

        ImVec2 GetDefaultSize() const override
        {
            return ImVec2( 1040.0f, 640.0f );
        }

        void OnUIRender() override;

        // What this window can be asked to do by something without a mouse. `Save` is the whole of it and
        // it is not optional: the graph is a FILE now, so an edit that is never written is an edit that
        // dies with the process — and a toolbar button cannot be pressed on this machine (synthetic input
        // is closed), which is what made the shader graph's own Save unphotographable until it became a
        // document action. This entry runs the SAME function the button runs.
        [[nodiscard]] std::vector<DocumentAction> Actions() override;

        // The entity, in the scene this document was opened over, still carrying an AnimationComponent.
        // All three have to hold: deleting the entity, removing the component, or closing the scene are
        // three ways for this window's subject to stop existing and the user experiences them as one.
        [[nodiscard]] bool IsSubjectAlive() const override
        {
            return ResolveComponent() != nullptr;
        }

        // A NODE CANVAS COSTS NO RENDERER SLOT. Everything this window draws is ImGui geometry; there is
        // no Scene, no SceneRenderer and no offscreen target, so it is not pending demand for one of the
        // six and closing it would free nothing. Answering the base class's conservative `true` would have
        // it refuse a sixth window over a slot it was never going to take.
        [[nodiscard]] bool HoldsRendererSlot() const override
        {
            return false;
        }

        [[nodiscard]] bool ClaimsRendererSlot() const override
        {
            return false;
        }

        // DELIBERATELY A NO-OP, AND NOT AN OVERSIGHT. The scene fanout (EditorLayer::SetActiveScene) exists
        // so the Outliner, Details and Settings follow whichever viewport has the focus. A document must
        // NOT follow it: its subject is an entity UUID, and UUIDs belong to one registry — repointing this
        // window at another scene would either find nothing or, worse, find a different entity that
        // happens to share the id. This window is about the entity it was opened on, in the scene it was
        // opened in, and when that scene goes away IsSubjectAlive says so and the document closes.
        void SetScene( const std::shared_ptr<Desert::Core::Scene>& /*scene*/ ) override
        {
        }

    private:
        // The component this window edits, or nullptr when it is gone. ONE resolution, used by the draw
        // and by the liveness answer, so "the window found something to draw" and "the subject is alive"
        // cannot disagree.
        [[nodiscard]] ECS::AnimationComponent* ResolveComponent() const;

        void DrawCanvas( ECS::AnimationComponent& anim, const std::vector<std::string>& clipNames );

        // The `.danimgraph` this entity names, or nullptr. ONE resolution, so "what the canvas draws" and
        // "what Save writes" can never be two different graphs.
        [[nodiscard]] Assets::Asset<Assets::AnimGraphAsset> ResolveAsset() const;

        // "The object you are holding was just changed." Bumps the ASSET's revision, which is what makes
        // every entity sharing this graph re-sync — the component-side `GraphRevision` this replaced could
        // only ever have re-synced the one entity whose window was open.
        void MarkEdited();

        // Writes the graph to its own file. Reports through the status line, which is this window's one
        // error channel.
        void SaveGraph();
        void DrawSidePanel( ECS::AnimationComponent& anim, const std::vector<std::string>& clipNames );

        // WEAK, not shared. A document that held its scene alive would keep a closed level in memory for
        // as long as its window was open, and — worse — would then answer "my subject is alive" about an
        // entity in a registry nothing else can reach. Expiry IS one of the ways this document's subject
        // dies, and a weak_ptr is what makes it visible rather than invisible.
        std::weak_ptr<::Desert::Core::Scene> m_Scene;
        const Animation::AnimationLibrary*   m_Library      = nullptr;
        Assets::AssetManager*                m_AssetManager = nullptr;
        std::string                          m_Status; // last save result line
        bool                                 m_StatusIsError = false;
        ax::NodeEditor::EditorContext*       m_Context = nullptr;

        bool m_ApplyPositions = true; // push State.X/Y into the canvas the first time this graph is drawn
    };
} // namespace Desert::Editor
