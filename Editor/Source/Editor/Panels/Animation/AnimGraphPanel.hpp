#pragma once

#include "../IPanel.hpp"

#include <Editor/Core/Commands/AnimGraphEdit.hpp>
#include <Editor/Core/SubjectEditorRegistry.hpp>
#include <Editor/Core/GraphCanvas/GraphCanvasView.hpp>
#include <Editor/Panels/Animation/AnimGraphCanvasPlan.hpp>
#include <Editor/Panels/Animation/PoseGraphEdit.hpp>

#include <Common/Core/UUID.hpp>

#include <cstdint>
#include <filesystem>

#include <glm/glm.hpp>

#include <Engine/Assets/Common.hpp>

#include <memory>
#include <optional>
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
namespace Desert::Animation::Graph
{
    struct AnimGraph;
    struct GraphWarning;
} // namespace Desert::Animation::Graph
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
    class PreviewViewport;
    namespace UI
    {
        class UIHelper;
    }

    // ── THE ANIM GRAPH OF ONE `.danimgraph`: A DOCUMENT, NOT A TOOL ──────────────────────────────────
    //
    // An imgui-node-editor canvas over ONE anim-graph asset — STATES are nodes, TRANSITIONS are
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
    // THE SUBJECT IS THE FILE (ANIM-FIX8), as in UE's Animation Blueprint Editor: the window is about the
    // `.danimgraph`, opened by double-clicking it in the Content Browser or from the Animation component's
    // Details button, and needs no entity in any scene. It used to be keyed by the entity whose
    // AnimationComponent named the graph — after the graph became an asset that was the wrong identity: two
    // characters on one graph opened two windows over the same file, and a graph no entity named could not
    // be opened at all. What the window drives is its own PREVIEW INSTANCE (UE: the preview AnimInstance):
    // an AnimationComponent the document owns, bound to this asset's shared graph.
    class AnimGraphPanel final : public ISubjectDocument
    {
    public:
        // The subject type this editor is registered under: the asset type, like every other asset editor —
        // so Core::RequestOpenAsset reaches it from a double-click and the Details button sends the same key.
        [[nodiscard]] static SubjectTypeKey SubjectType()
        {
            return AssetSubjectType( static_cast<uint32_t>( Assets::AssetTypeID::AnimGraph ) );
        }

        // The subject for ONE `.danimgraph` — what a Details button sends (and the browser, through the
        // asset route, arrives at).
        [[nodiscard]] static SubjectId SubjectFor( const Assets::AssetHandle graph )
        {
            return AssetSubject( graph, static_cast<uint32_t>( Assets::AssetTypeID::AnimGraph ) );
        }

        AnimGraphPanel( const SubjectId& subject, const std::string& displayName,
                        Animation::AnimationLibrary* library, Assets::AssetManager* assetManager );
        ~AnimGraphPanel() override;

        [[nodiscard]] glm::vec2 GetDefaultSize() const override
        {
            return { 1040.0f, 640.0f };
        }

        void OnUIRender() override;

        // What this window can be asked to do by something without a mouse. `Save` is the whole of it and
        // it is not optional: the graph is a FILE now, so an edit that is never written is an edit that
        // dies with the process — and a toolbar button cannot be pressed on this machine (synthetic input
        // is closed), which is what made the shader graph's own Save unphotographable until it became a
        // document action. This entry runs the SAME function the button runs.
        [[nodiscard]] std::vector<DocumentAction> Actions() override;

        // The `.danimgraph` is still registered: deleting the file is the way this window's subject stops
        // existing. Same answer as the registration's liveness test, and ResolveComponent rests on it.
        [[nodiscard]] bool IsSubjectAlive() const override;

        // THE PREVIEW VIEWPORT COSTS ONE RENDERER SLOT (UE: the Animation Blueprint Editor's viewport). The
        // window holds a PreviewViewport — a SceneRenderer — from its first drawn frame until ReleaseView,
        // which destroys it; the canvas and the side panel keep working without it on the document's own
        // instance. Same contract as the Animation Editor (AnimationEditorIdentity).
        [[nodiscard]] bool HoldsView() const override
        {
            return m_Preview != nullptr;
        }
        void               ReleaseView() override;
        [[nodiscard]] bool HasPreview() const override
        {
            return true;
        }
        void SetPreviewViewpoint( const PreviewViewpoint& viewpoint ) override;
        void OnPreUpdate() override;

        // A NO-OP: the scene fanout (EditorLayer::SetActiveScene) is for the Outliner, Details and Settings.
        // This window's subject is a file, which belongs to no scene, so there is nothing to repoint.
        void SetScene( const std::shared_ptr<Desert::Core::Scene>& /*scene*/ ) override
        {
        }

    private:
        // The preview instance this window drives, bound to the asset's shared graph (`Graph` re-pointed at
        // every resolution, so a reload of the file is followed), or nullptr when the asset is gone or not
        // loaded. ONE resolution, used by the draw and the actions.
        [[nodiscard]] ECS::AnimationComponent* ResolveComponent() const;
        /// The preview character's mesh (null until there is one): the mesh whose skeleton reference decides
        /// which clips this graph's states can name.
        [[nodiscard]] Assets::AssetHandle ResolveMeshHandle() const;

        /// @p width and @p height are passed rather than taken from the child window that used to wrap
        /// this: see the note at the call site, and `Graph::DeferredFrameAll` for what the child was
        /// costing. The height is given because the warning strip below the canvas has to be reserved
        /// BEFORE the canvas is drawn, and a canvas that took "the rest of the window" would sit on it.
        void DrawCanvas( ECS::AnimationComponent& anim, float width, float height );

        /// The ⚠ strip of §8.2: what `Animation::Graph::Validate` found, drawn where a person authoring
        /// the graph is looking. Returns nothing — it is the LAST thing drawn — and takes the findings
        /// rather than the graph, because the deciding belongs to a unit with no ImGui in it.
        void DrawWarningStrip( const Animation::Graph::AnimGraph&                 graph,
                               const std::vector<Animation::Graph::GraphWarning>& warnings );

        /// The document action behind ONE finding: re-resolves the component and reveals @p warning.
        /// A MEMBER and not a lambda — see the call site for why the check leaves no lambda available.
        /// BY REFERENCE, and `bind_front` is what makes that safe: it stores its own COPY of the bound
        /// finding, so the reference this sees names that copy and not the vector the palette built from.
        void RevealFinding( const Animation::Graph::GraphWarning& warning );

        /// The clip names this entity's skeleton can actually play, empty when there is no Animator to ask
        /// yet. ONE derivation, shared by the clip picker, the validator and the document actions: this
        /// picker used to ask the library tolerantly while the state machine asked it exactly, so a clip
        /// offered here resolved to nothing at runtime and the state played nothing without a word.
        [[nodiscard]] std::vector<std::string> ResolveClipNames( const ECS::AnimationComponent& anim ) const;

        /// Selects on the canvas whatever @p warning is about, and moves the view to it. What turns the
        /// strip from a wall of text into a way to reach the control that fixes the finding — and what
        /// finally gives `GraphWarning::State` and `::Transition` a reader; they were computed for every
        /// finding and read by nothing at all.
        void RevealWarning( const Animation::Graph::AnimGraph&    graph,
                            const Animation::Graph::GraphWarning& warning );

        /// The height `DrawWarningStrip` will take for @p count findings, so the canvas above it can be
        /// made that much shorter. One function answers both questions, because a reserved height and a
        /// drawn height that are computed separately are two numbers that drift by a pixel a release.
        [[nodiscard]] static float WarningStripHeight( size_t count );

        // The `.danimgraph` this window is about (its subject), or nullptr. ONE resolution, so "what the canvas
        // draws" and "what Save writes" can never be two different graphs.
        [[nodiscard]] Assets::Asset<Assets::AnimGraphAsset> ResolveAsset() const;

        // "The object you are holding was just changed." Bumps the ASSET's revision, which is what makes
        // every entity sharing this graph re-sync — the component-side `GraphRevision` this replaced could
        // only ever have re-synced the one entity whose window was open.
        void MarkEdited();

        /// This window's graph as an undo owner: the asset resolved by handle at every use, so an entry outlives
        /// a reload of the file; a restore bumps the asset's revision (every entity re-syncs).
        [[nodiscard]] AnimGraphOwner GraphOwner() const;

        // Writes the graph to its own file. Reports through the status line, which is this window's one
        // error channel.
        void SaveGraph();

        /// Appends a state, named so that nothing else in this graph carries that name and placed where
        /// nothing else in this graph already sits. Shared by the toolbar button and by the document
        /// action of the same name, so what a client drives is what a person presses.
        void AddState();
        /// Appends a parameter named so that nothing else in this graph carries that name. Shared by the
        /// toolbar button and by the document action of the same name, for the reason `AddState` is.
        void AddParameter();
        /// @p height is the canvas's, so the two columns end on the same line and neither of them sits
        /// on the warning strip below. See the call site.
        void DrawSidePanel( ECS::AnimationComponent& anim, const std::vector<std::string>& clipNames,
                            float height );

        // ── THE POSE GRAPH (UE's AnimGraph tab): the graph's own nodes on a canvas of their own ─────────
        //
        // Every edit is ONE function of `PoseGraphEdit` (no ImGui, measured by AnimGraphValidation), called
        // alike by the canvas drag, the context menu and the document action of the same name; a refusal
        // goes to the status line with the unit's sentence, and the graph is left as it was.

        /// Adds a node of @p kind: at @p where (canvas coordinates) from the context menu, on the free grid
        /// cell from a document action. A Sequence Player starts on the skeleton's first playable clip.
        void AddPoseNode( Animation::Graph::PoseNodeKind kind, const std::optional<glm::vec2>& where );
        /// Wires @p from into Pose pin @p pin of @p to; @p to empty means Output Pose.
        void WirePose( const std::string& from, const std::string& to, int pin );
        void RemovePoseNode( const std::string& name );
        /// Reports a refused edit on the status line. Returns whether the edit went through.
        bool Report( const Common::BoolResultStr& result );
        /// The pose graph the canvas shows: the host's nodes and Output Pose, or a layer graph's. A layer
        /// that no longer exists (renamed, undone) falls back to the host. REFERENCES, not pointers: there is
        /// always a graph to show (the fallback), and the view lives no longer than the call that resolved it.
        struct PoseGraphTarget
        {
            std::vector<Animation::Graph::PoseNode>& Nodes;
            std::string&                             Output;
            float&                                   OutputX; // the Output Pose node's canvas position
            float&                                   OutputY;
            Animation::Graph::GraphScope             Scope = Animation::Graph::GraphScope::Host;
        };
        [[nodiscard]] PoseGraphTarget ResolvePoseTarget( Animation::Graph::AnimGraph& graph );
        /// The machine the state canvas edits (m_MachineNode in the shown pose graph, else the host's Output
        /// Pose machine), or nullptr.
        [[nodiscard]] Animation::Graph::StateMachine* ResolveMachine( Animation::Graph::AnimGraph& graph );
        /// The canvas on screen: the state machine's or the pose graph's. The view controls (Frame All, Frame
        /// Selection, the toolbar buttons) act on THIS one, never on a hidden canvas.
        [[nodiscard]] ax::NodeEditor::EditorContext* ShownCanvas() const
        {
            return m_EditingMachine ? m_Context : m_PoseContext;
        }
        /// Show the host's AnimGraph (empty) or a layer graph; the canvas ids and selection start over.
        void ShowPoseGraph( std::optional<std::pair<std::string, std::string>> layer );
        /// Open the state machine node `node` of the shown pose graph on the state canvas.
        void OpenMachine( const std::string& node );
        void AppendPoseActions( ECS::AnimationComponent& anim, std::vector<DocumentAction>& actions );
        void DrawPoseCanvas( ECS::AnimationComponent& anim, float width, float height );
        void DrawPoseSidePanel( ECS::AnimationComponent& anim, const std::vector<std::string>& clipNames,
                                float height );
        /// A parameter pin's binding as a combo of the declared parameters ("unbound" = the pin's default).
        bool DrawPinBinding( Animation::Graph::AnimGraph& graph, Animation::Graph::PoseNode& node,
                             const std::string& pin );

        // THE PREVIEW INSTANCE (UE: the editor's preview AnimInstance): owned by this document, never a
        // scene entity's component — editing a graph must not need, or disturb, a character in the level.
        // Its `GraphAsset` is the subject and its `Graph` the asset's shared object.
        /// The preview pane: created on the first frame the window is drawn, its target given the chosen
        /// skeletal mesh and an AnimationComponent naming this graph (PreviewViewport::SetGraphCharacter).
        void EnsurePreview();
        void SetPreviewMesh( size_t candidate );
        void TogglePreview();
        void DrawPreviewPane( float width, float height );

        std::unique_ptr<PreviewViewport>   m_Preview;
        std::unique_ptr<UI::UIHelper>      m_UIHelper;
        std::vector<std::filesystem::path> m_MeshCandidates; // every registered .skmesh (ContentRegistry rows)
        size_t                             m_MeshIndex = 0;
        Assets::AssetHandle                m_PreviewMesh{ static_cast<uint64_t>( 0 ) };
        glm::uvec2                         m_RenderSize{ 0u };
        bool                               m_DrewThisFrame = false;
        std::optional<glm::vec2>           m_PendingOrbitDegrees;
        // The instance the canvas edits while no preview character exists (before the first frame, after
        // ReleaseView, headless): bound to the same shared graph. Once the preview exists, ResolveComponent
        // answers the preview target's AnimationComponent — the one the AnimationECSSystem evaluates.
        std::unique_ptr<ECS::AnimationComponent> m_Instance;
        Animation::AnimationLibrary*             m_Library      = nullptr;
        Assets::AssetManager*                m_AssetManager = nullptr;
        std::string                          m_Status; // last save result line
        bool                                 m_StatusIsError = false;

        // THE ● OF THE §8.2 HEADER: the asset revision as of the last write to disk. Unset until the
        // first frame that resolves the asset (it may still be loading at construction). The ASSET's revision and
        // not a flag of this window's own, so that an edit made in a second window over the SAME .danimgraph also
        // shows here — which is the whole point of a graph being one shared object (§5.1).
        //
        // IT IS CONSERVATIVE IN ONE DIRECTION, said out loud: `AnimGraphAsset::Load` bumps the revision
        // too, so re-loading the file from disk under an open window shows the dot until the next Save.
        // That errs towards asking for a write that is not needed, never towards hiding one that is.
        std::optional<uint32_t>              m_SavedRevision;
        ax::NodeEditor::EditorContext*       m_Context = nullptr;

        // CANVAS IDENTITY, AND IT IS NOT AN INDEX. `NodeId( i ) = i + 1` meant that deleting a state
        // shifted every later state onto its neighbour's id, so the canvas handed back the neighbour's
        // position and the panel wrote it into the wrong state — one deletion moved the whole layout.
        // The map issues an id per state NAME, which is the identity the graph already resolves by.
        Graph::ElementIdMap    m_Ids;
        Graph::AnimGraphCanvas m_Canvas; // this frame's plan; the side panel reads it to map a selection

        // Framing the content waits for a canvas that has stopped resizing — it does not exist on the
        // first frame, and a navigation issued while it is still changing size is thrown away. The whole
        // measurement is at the class's declaration.
        Graph::DeferredFrameAll m_FrameAll;

        // The pose graph's canvas: its own editor context and id table, because a state and a pose node may
        // carry one name and must not share a canvas identity (nor a view: each canvas keeps its own pan).
        ax::NodeEditor::EditorContext* m_PoseContext = nullptr;
        Graph::ElementIdMap            m_PoseIds;
        Graph::PoseGraphCanvas         m_PoseCanvas;
        Graph::DeferredFrameAll        m_PoseFrameAll;
        /// Which canvas is shown: the AnimGraph (pose graph, UE's default tab) or the Output Pose's state
        /// machine (double-click its node, as in UE).
        bool m_EditingMachine = false;
        /// WHICH GRAPH THE POSE CANVAS SHOWS (UE: the AnimGraph tab or a layer function graph): empty = the
        /// graph's own AnimGraph (GraphScope::Host), else (interface, layer) of a layer graph it implements
        /// (GraphScope::Layer — where Linked Input Pose may be added).
        std::optional<std::pair<std::string, std::string>> m_LayerGraph;
        /// WHICH MACHINE THE STATE CANVAS EDITS: a state machine node of the shown pose graph, by name
        /// (double-click it, as in UE); empty = the machine at the host's Output Pose.
        std::string m_MachineNode;
        std::string m_SelectedPoseNode;
        bool        m_PoseSelectPending = false; // a document action picked the node
        glm::vec2   m_PoseMenuAt{};              // where the context menu was opened

        // EVERY EDIT OF THE GRAPH IS ONE UNDO ENTRY (UE: FScopedTransaction on the AnimBlueprint): discrete edits
        // open a Scope, the canvas and the side panel are observed once per frame (a drag = one entry).
        AnimGraphEditTransaction m_GraphEdit;
        // The asset revision this window last wrote or saw. A move it did not make (Undo, Redo, another window)
        // re-issues both canvases' ids, so the restored X/Y are pushed into the node editor, not pulled over.
        std::optional<uint32_t> m_SeenRevision;
    };
    // The `.danimgraph` path opener (UE: double-click an Animation Blueprint in the Content Browser):
    // find-or-create the AnimGraphAsset, load it, then open it through the one handle route,
    // Core::RequestOpenAsset. Any other extension is NotMine.
    [[nodiscard]] SubjectEditorRegistry::PathOpenOutcome
    RequestAnimGraphDocument( Assets::AssetManager* assets, const std::string& path,
                              const SubjectEditorRegistry& editors );
} // namespace Desert::Editor
