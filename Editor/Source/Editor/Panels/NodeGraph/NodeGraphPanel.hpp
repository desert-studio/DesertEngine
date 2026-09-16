#pragma once

#include "../IPanel.hpp"
#include "ShaderGraph.hpp"
#include "ShaderGraphCanvasPlan.hpp"

#include <Editor/Core/GraphCanvas/GraphCanvasView.hpp>

#include <Engine/Assets/Common.hpp>
#include <Engine/Assets/ShaderGraphAsset.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ax::NodeEditor
{
    struct EditorContext;
}

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Editor
{
    // ── THE SHADER GRAPH OF ONE `.dgraph`: A DOCUMENT, AND U7-2's REFUSAL IS SPENT ────────────────────
    //
    // An interactive node canvas (imgui-node-editor) over ONE `ShaderGraphAsset` that COMPILES to a Desert
    // Shader Language file. Compile writes Resources/Shaders/Programs/Graph/<Name>.shader and registers it
    // with the ShaderService, so the shader immediately appears in the material shader picker; recompiles
    // of an already registered graph go through the normal shader hot-reload.
    //
    // IT USED TO BE THE EDITOR'S LAST TOOL-THAT-EDITS-SOMETHING. U7 moved the material, the four cloud
    // stages, the anim graph and the particle emitter into documents; U7-2 moved the UI canvas and the two
    // timelines and MEASURED why this one could not follow, naming four obstacles. Three of them were
    // "there is no `AssetTypeID::ShaderGraph`", and they are gone with the type. The fourth was the one
    // that cost work: `New`, `Load` and the browser's double-click all replaced `m_Doc` unconditionally, so
    // opening a second graph discarded unsaved edits with no prompt. That is no longer expressible — one
    // window is one graph, fixed at construction, and a second graph is a second window.
    //
    // THE ONE OBSTACLE THAT WAS FALSE, recorded because it was the expensive-looking one: "a `.dgraph` has
    // no identity of its own, so the FORMAT gains a field and the graphs in the tree gain a migration".
    // `Common::AssetHandle` is derived from the project-relative PATH, deterministically, without reading
    // the payload — which is how `.derig` and `.detheme` have stable handles carrying no id either. Zero
    // `.dgraph` files changed.
    //
    // WHAT WENT AWAY WITH THE TOOL: the `Load ▾` popup that enumerated the graphs folder. Opening a graph
    // is the asset browser's double-click and the command palette's Open group — both of which work on any
    // `.dgraph` now, because the path opener claims the extension, which is the same change that made this
    // window drivable without a mouse at all.
    class NodeGraphPanel final : public ISubjectDocument
    {
    public:
        // The subject type this editor is registered under. Named here rather than spelled at the
        // registration and at the path opener separately.
        [[nodiscard]] static SubjectTypeKey SubjectType()
        {
            return AssetSubjectType( static_cast<uint32_t>( Assets::AssetTypeID::ShaderGraph ) );
        }

        NodeGraphPanel( const Assets::AssetHandle& graph, const std::string& displayName,
                        const std::shared_ptr<Assets::AssetManager>& assetManager );
        ~NodeGraphPanel() override;

        ImVec2 GetDefaultSize() const override
        {
            return ImVec2( 1100.0f, 680.0f );
        }

        void OnUIRender() override;
        void OnPreUpdate() override;

        // The `.dgraph` this window was opened on, still known to the asset manager. Deleting the file's
        // record is how this subject stops existing, and the document goes with it.
        [[nodiscard]] bool IsSubjectAlive() const override;

        // A NODE CANVAS COSTS NO RENDERER SLOT — everything here is ImGui geometry. Same answer, and the
        // same reason, as AnimGraphPanel's.
        [[nodiscard]] bool HoldsRendererSlot() const override
        {
            return false;
        }

        [[nodiscard]] bool ClaimsRendererSlot() const override
        {
            return false;
        }

        // What this window can be asked to do by something without a mouse. These three are the whole of
        // obstacle #4: before the window was a document, Save / Compile / Frame were buttons on a toolbar,
        // and a toolbar button cannot be pressed on this machine (synthetic input is closed) and cannot be
        // photographed. Every entry here runs the SAME function the button runs — not a second path.
        [[nodiscard]] std::vector<DocumentAction> Actions() override;

        // Creates a starter `.dgraph` (unique name) in @p directory and returns its path — the File
        // Explorer's "New Shader Graph" context action, and the toolbar's `New`. EMPTY when the file could
        // not be written (the reason is logged); the path is the caller's proof that there is something to
        // open, so an empty one must not be passed on.
        [[nodiscard]] static std::string
        CreateNewGraphFile( const std::string&  directory,
                            ShaderGraph::Domain domain = ShaderGraph::Domain::Surface );

    private:
        void ChangeDomain( ShaderGraph::Domain domain ); // swaps the output node, prunes off-domain nodes
        void DrawToolbar();
        void DrawCanvas();
        void SaveGraph();
        void Compile();

        // The asset this window is bound to, or nullptr when it is gone. ONE resolution, used by the draw
        // and by the liveness answer, so "the window found something to draw" and "the subject is alive"
        // cannot disagree.
        [[nodiscard]] Assets::Asset<Assets::ShaderGraphAsset> ResolveAsset() const;

        const ShaderGraph::Pin* FindPin( uint64_t id ) const;
        bool                    IsInputPin( uint64_t id ) const;

        // Makes sure a material asset exists that uses this graph's shader, and returns its handle. The
        // Material Editor is a MATERIAL editor, so a bare shader is not something it can show.
        Assets::AssetHandle EnsurePreviewMaterial();

        // Hand the freshly compiled shader to the Material Editor: tell every window on this shader to drop
        // the pipelines it cached from the old modules, then open-or-focus this graph's material.
        void PublishToPreview();

        std::shared_ptr<Assets::AssetManager> m_AssetManager;
        ax::NodeEditor::EditorContext*        m_Context = nullptr;

        // WHERE THE GRAPH IS WRITTEN, resolved ONCE from the subject rather than composed from the
        // document's Name on every Save. Save used to build `ShaderGraphs/<Name>.dgraph`, which made
        // renaming a graph a silent Save As into a second file while this window still claimed to be the
        // first one. A document saves to the file it was opened on.
        std::filesystem::path m_Path;

        ShaderGraph::Document m_Doc;
        std::string           m_Status; // last save/compile result line
        bool                  m_StatusIsError = false;

        // WHICH ELEMENTS THE CANVAS ALREADY KNOWS. This document issues its own ids (`Document::NextId`),
        // so the shared layer is not asked to name anything here — only to remember what it has seen, so
        // that a node's stored X/Y is pushed in exactly once. `m_ApplyPositions` was one bool for the
        // whole document, which is why creating a node from the palette needed a SECOND
        // `ed::SetNodePosition` at the creation site; per element there is no such special case.
        Graph::ElementLedger    m_Ledger;
        Graph::CanvasPlan       m_Plan;
        Graph::DeferredFrameAll m_FrameAll;

        // The material this graph's shader is previewed on, created on the first successful Compile and
        // reused after that (a new asset per compile would litter the project with scratch materials).
        Assets::AssetHandle m_PreviewMaterial{ static_cast<uint64_t>( 0 ) };

        // Recompile once editing has SETTLED. See AutoCompileIfSettled for where the number comes from:
        // a graph rebuild costs roughly one of these, measured, so a settled graph reaches the preview in
        // about the time a rebuild takes while a dragged edit produces one compile instead of a dozen.
        static constexpr std::chrono::milliseconds kAutoCompileDelay{ 400 };

        // A signature of what the graph MEANS — node kinds, constant values and links, never positions.
        static uint64_t StructuralFingerprint( const ShaderGraph::Document& doc );
        void            AutoCompileIfSettled();

        bool                                  m_AutoCompile     = true;
        uint64_t                              m_LastFingerprint = 0;
        std::chrono::steady_clock::time_point m_DirtySince{}; // default = nothing pending
    };
} // namespace Desert::Editor
