#pragma once

#include "../IPanel.hpp"

#include "MaterialEditStates.hpp"

#include <Editor/Widgets/PreviewViewport.hpp>
#include <Editor/Widgets/UIHelper/ImGuiUI.hpp>

#include <Engine/Assets/Common.hpp>
#include <Engine/Assets/MaterialData.hpp>
#include <Engine/Core/Formats/ShaderProgramMeta.hpp>

#include <memory>
#include <optional>
#include <string>

namespace Desert::Assets
{
    class AssetManager;
    class SurfaceMaterialAsset;
}

namespace Desert::Graphic
{
    class ImageCube;
}

namespace Desert::Editor
{
    // THE MATERIAL EDITOR, one window per `.demat`, opened by double-clicking the asset — UE's flow. The
    // material is drawn live on a primitive under a real key light and a procedural sky, orbitable, with its
    // parameters beside it.
    //
    // WHAT REPLACED WHAT. This is MaterialPreviewPanel, which was a SINGLETON with a material combo — one
    // window, whichever material was picked in it. UE has no such thing and neither do we any more: the
    // subject is fixed at construction, two materials are two windows, and the combo is gone with the panel.
    //
    // WHY IT CANNOT FLATTER. PreviewViewport::SetMaterial fills StaticMeshComponent::MaterialSlots — the
    // per-SLOT route, the one a real scene mesh takes. The 128 px thumbnail this lineage replaced went
    // through MaterialComponent::ShaderName, the shader-OVERRIDE route, which MeshRenderer re-seeds with
    // schema defaults every frame; that is how the old preview showed a correct material while the scene
    // showed black (Docs/MaterialEditor/STAGE1_END_TO_END.md). Desert/Tests/Editor/MaterialPreviewRoute
    // guards the distinction and fails in BOTH directions.
    //
    // COST WHEN CLOSED IS ZERO, not "small". The PreviewViewport — and with it a Scene, a SceneRenderer and
    // one of the six renderer slots — is created on the first frame the window actually draws, and released
    // when the window is DISMISSED, which destroys this panel outright (EditorLayer::
    // ServiceDocumentCloses). That is the difference between a document and a tool panel: a tool is
    // hidden and kept, so it has to be told to let go of its renderer; a document ceases to exist, so it
    // cannot forget to.
    class MaterialEditorPanel final : public ISubjectDocument
    {
    public:
        MaterialEditorPanel( const Assets::AssetHandle&                   material,
                             const std::shared_ptr<Assets::AssetManager>& assetManager );
        ~MaterialEditorPanel() override;

        ImVec2 GetDefaultSize() const override
        {
            return ImVec2( 760.0f, 560.0f );
        }

        void OnUIRender() override;
        void OnPreUpdate() override;

        [[nodiscard]] bool HoldsRendererSlot() const override
        {
            return m_Preview != nullptr;
        }

        // The `.demat` this window is about, gone from the manager — deleted in the browser, or the project
        // closed under it. Read through the same resolution the pane draws from, so "the document says its
        // subject is alive" and "the pane found something to draw" cannot disagree.
        [[nodiscard]] bool IsSubjectAlive() const override
        {
            return ResolveSubject() != nullptr;
        }

        // The preview — a Scene, a SceneRenderer and one of the six slots — while this window is not on
        // screen. ReleasePreview is what a close already does; this is the same teardown reached because
        // nobody is looking, and OnPreUpdate builds it back on the first frame the window is drawn again.
        void ReleaseRendererSlot() override
        {
            ReleasePreview();
        }

        // NOT ALWAYS — and that is the point. A material whose shader draws no mesh geometry (a
        // Terrain-domain one; see PreviewUnavailableReason) never builds a PreviewViewport at all, so it
        // is not demand for a renderer slot that has yet to land. Answering the base class's `true` here
        // would make such a window count against the six for ever, and the census would tell the user to
        // close a window that holds nothing and never will — the exact failure the four cloud documents
        // caused before ISubjectDocument::ClaimsRendererSlot existed.
        //
        // Read from the same string the pane prints, so the census and what the artist is looking at
        // cannot disagree. Empty before the first draw, which is the conservative answer: a window that
        // has not decided yet is counted as a claimant.
        [[nodiscard]] bool ClaimsRendererSlot() const override
        {
            return m_PreviewUnavailable.empty();
        }

        // THE PREVIEW CAN BE AIMED, and the command palette is what aims it — the replacement for
        // `--preview-orbit yaw,pitch`, which existed only because macOS refuses this machine synthetic
        // input and "the same ball from the other side" is what separates an object from its background.
        // A named viewpoint is the same evidence and reproduces without anybody remembering two numbers.
        [[nodiscard]] bool HasPreview() const override
        {
            return m_Preview != nullptr;
        }

        void SetPreviewViewpoint( const PreviewViewpoint& viewpoint ) override;

        // ── The three states (ISubjectDocument) ───────────────────────────────────────────────────────
        //
        // This document STAGES. An edit lands in a working copy that only the pane beside it draws; the
        // scene keeps rendering the state somebody accepted, until Apply. See PublishToRuntime for the
        // mechanism and EnsureWorkingCopy for why the copy has to be a second material asset.
        [[nodiscard]] EditModel GetEditModel() const override
        {
            return EditModel::Staged;
        }

        [[nodiscard]] bool      HasUnappliedEdits() const override;
        bool                    ApplyEdits() override;
        bool                    DiscardEdits() override;
        [[nodiscard]] DiskState GetDiskState() const override;

        // SAVE IMPLIES APPLY, and the order is not a convenience. Saving publishes the working copy first
        // and writes second, so the file and the running editor can never disagree — a Save that wrote the
        // working values while the scene kept rendering the older ones would put the third state ahead of
        // the second, and the next person to look at the level would see a material that does not match
        // its own file. Returns whether the file was written.
        bool SaveDocument() override;

        // ── The properties, for the control channel (ISubjectDocument) ────────────────────────────────
        //
        // DERIVED FROM THE SHADER'S SCHEMA, through MaterialEdit::DescribeProperties — the same walk of
        // the same `Properties` block that DrawParameters builds its rows from. There is no list of
        // parameter names anywhere in this window, and that is deliberate: a third one beside the schema
        // and the table would drift, and what a client could set would stop being what an artist can
        // edit.
        [[nodiscard]] std::vector<EditableProperty> EditableProperties() const override;

        // Reaches the material through WriteParam, which is the same function the slider calls.
        [[nodiscard]] Common::BoolResultStr SetEditableProperty( const std::string&        name,
                                                                 const std::vector<float>& value ) override;

    private:
        // @p working is the document's working copy, or null while there is none — the material is not
        // loaded, or the copy was refused. The view controls still draw; the actions that would change or
        // write a material are not offered rather than offered and doing nothing.
        void DrawToolbar( Assets::SurfaceMaterialAsset* working, bool isInstance );

        // Unity-style shader picker inside the material. Base assets only: an instance always renders with
        // its parent chain's shader, so a picker on one would be a control with nothing behind it.
        // Returns true when the shader changed (the runtime material is a different CLASS and must be
        // rebuilt, not merely re-valued).
        //
        // THE LIST IS THIS MATERIAL'S OWN DOMAIN, not a fixed one. It used to filter on a hardcoded
        // `Domain != Surface`, so a Terrain material displayed `Terrain` over a list that could not
        // contain it: the one value the combo was showing was the one value it could not reproduce, and
        // any click at all moved the material into another domain — where its parameters name uniform
        // fields the new shader does not have. Nothing downstream rejects that mismatch (it builds a
        // valid pipeline and draws wrong), so the picker is the only place it can be prevented.
        bool DrawShaderPicker( Assets::SurfaceMaterialAsset& asset );

        // The schema-driven parameter editor — every parameter the shader declares, TEXTURES INCLUDED.
        // Moved here from the Details panel's material fold (Docs/MaterialEditor/
        // PLAN_STAGE3_ASSET_DOCUMENTS.md, M2): authoring a material is this window's job, and until the
        // move a texture could only be bound in Details because the window's own table skipped texture
        // params. Returns true when anything changed.
        //
        // @p parentData / @p isInstance: material-INSTANCE mode — the schema comes from the parent's
        // shader, non-overridden rows display the PARENT's value, edits write overrides into the child,
        // and texture rows are read-only (per-instance texture descriptors are a v2).
        /// One non-texture asset-reference row of the schema (CloudType / CloudLayout — the kinds the
        /// Volume domain declares). Combo + AssetFile drag-drop, value in MaterialData::Textures by name.
        bool DrawCloudAssetRef( Assets::MaterialData& data, const ::Desert::Core::Formats::ShaderParam& p,
                                const std::string& hiddenId );

        bool DrawParameters( Assets::SurfaceMaterialAsset& asset, const Assets::MaterialData* parentData,
                             bool isInstance );

        // The parent of an instance subject, or null for a base material (and for an instance whose parent
        // no longer resolves — the caller shows the schema defaults rather than inventing values).
        [[nodiscard]] std::shared_ptr<Assets::SurfaceMaterialAsset>
        ResolveParent( const Assets::SurfaceMaterialAsset& asset ) const;

        // Push @p asset's current values at every runtime material already built from it, so that whatever
        // is rendering that asset shows them on the next frame.
        //
        // IT IS CALLED WITH TWO DIFFERENT ASSETS AND THAT IS THE WHOLE DESIGN. Called with the WORKING
        // COPY it moves the ball in this window and nothing else; called with the SUBJECT (from ApplyEdits)
        // it moves every mesh in every open scene. One mechanism, two audiences, and which audience is
        // reached is decided by which asset is handed in rather than by a flag inside it.
        //
        // It used to be `PropagateEdit`, called with the subject from the edit sites themselves — one wire
        // to both audiences, which is why an accidental drag changed the level.
        void PublishToRuntime( Assets::SurfaceMaterialAsset& asset, bool isInstance );

        // Write the subject back to its `.demat` and drop the thumbnail rendered from the old values.
        // Returns whether the file was written: the on-disk snapshot must not move when it was not.
        bool SaveSubject( Assets::SurfaceMaterialAsset& asset );

        // The subject material, or null when it is no longer loaded (deleted on disk, project reloaded).
        [[nodiscard]] std::shared_ptr<Assets::SurfaceMaterialAsset> ResolveSubject() const;

        // The document's WORKING COPY, created on the first frame the subject resolves, or null while
        // there is none — the subject is not loaded yet, or making the copy was refused (which is logged
        // once and written into m_WorkingCopyRefusal for the window to say out loud).
        //
        // A SECOND MATERIAL ASSET, registered beside the subject. SurfaceMaterialAsset::CreateWorkingCopy
        // carries the argument for why it cannot be anything smaller; the short form is that a material
        // reaches the screen as one runtime object cached under the asset's handle, so two audiences
        // seeing two different states needs two assets.
        [[nodiscard]] Assets::SurfaceMaterialAsset* EnsureWorkingCopy( Assets::SurfaceMaterialAsset& subject );

        // The material this window DRAWS AND EDITS: the working copy once there is one, and the subject
        // before the first frame that could make it (and after a refusal, where the window is read-only
        // and says so). Every per-domain decision — the schema the parameter table walks, which draw fills
        // the pane, what the preview's pushed identity is — reads THIS, so the pane and the table cannot
        // answer from two different materials within one frame.
        [[nodiscard]] std::shared_ptr<Assets::SurfaceMaterialAsset> DrawnMaterial() const;

        // The two "dirty"s, derived from the three states. See MaterialEditStates.hpp for why they are two
        // and why neither is a remembered flag.
        [[nodiscard]] MaterialEdit::DirtyState Dirty() const;

        // THE SCHEMA THIS WINDOW EDITS AGAINST, or null while its shader is not loaded. One resolution,
        // read by the parameter table, by the property census and by a `set` from the control channel —
        // so the rows a person sees, the list a client is offered and the names a write is checked
        // against are one walk of one declaration. Three resolutions would be three lists.
        //
        // An INSTANCE resolves through its parent, because EffectiveShaderName() does.
        [[nodiscard]] const ::Desert::Core::Formats::ShaderProgramMeta* Schema() const;

        // THE MERGED SCHEMA'S STORAGE, and it exists because Schema() answers a POINTER that outlives the
        // call. For everything but a cloud material the answer is the shader's own meta, owned by the
        // shader; for a cloud material whose Medium slot names a graph with properties of its own, the
        // answer is that meta PLUS those properties, which is an object nobody owns yet.
        //
        // Rebuilt inside Schema() rather than latched on a change, for the reason the cloud renderer
        // resolves its material every frame: the medium can be re-picked, hot-reloaded or edited under the
        // same handle, and a cache keyed on anything less than the schema itself is a row that stops
        // appearing. It is a vector copy of a few dozen small structs, once per frame, in a window that is
        // drawing ImGui.
        mutable ::Desert::Core::Formats::ShaderProgramMeta m_MergedSchema;

        // THE ONE WRITE. Every value that reaches this material's working copy goes through here: the
        // slider in DrawParameters and a `set` arriving on the control channel alike.
        //
        // Not two functions doing the same thing to the same field. The rule the control channel is built
        // on is that there is no second EXECUTION path — a channel that wrote the value itself would be
        // correct on the day it was written and wrong on the day somebody adds a step here (the publish is
        // already one; an undo entry will be the next), and the failure would be a scripted capture that
        // proves a route nobody uses.
        //
        // Returns false only when there is nothing to write into (no working copy, no schema); the caller
        // that came from the channel turns that into a refusal with a reason, and the widget cannot reach
        // it at all because it is disabled in exactly that state.
        bool WriteParam( const ::Desert::Core::Formats::ShaderParam& p, const glm::vec4& value );

        // THE ONE UNWRITE, and the pair of WriteParam above — same working copy, same publish, same
        // reasoning about why there is exactly one of it.
        //
        // IT IS AN EDIT AND IT BEHAVES LIKE EVERY OTHER EDIT, which is the lesson Д29 paid for in Details:
        // a reset that changes memory and tells nobody evaporates with the session. Here "tell somebody"
        // means landing in the WORKING copy, because this document's dirty marks are DERIVED from it
        // (MaterialEdit::EvaluateDirty) — so the tab's dot, the close prompt, Apply and Discard all learn
        // about a reset for free, and cannot be forgotten at this call site.
        //
        // There is deliberately NO CommandHistory entry, and that is not this control being special: this
        // window records no undo for ANY of its edits (every slider goes through WriteParam, which pushes
        // nothing), and Discard is its way back. A reset that was the one undoable action in a window of
        // non-undoable ones would be a worse lie than none at all.
        //
        // Returns whether anything was removed — false when the row was already showing what it inherits,
        // exactly like ResetFieldToDefault. Nothing is published for a reset that changed nothing.
        bool ResetParam( const ::Desert::Core::Formats::ShaderParam& p );

        void EnsurePreview();  // create the viewport + scene + renderer (claims a slot)
        void ReleasePreview(); // destroy them (returns the slot)

        // Why this material cannot fill the preview pane, or empty while it can.
        //
        // Two domains fill it today, each with its own draw (the pane's extension point is "what fills
        // the pane", not the Shape list — see PreviewViewport::SetCubemapMaterial): Surface rides a
        // sphere/cube/plane through the ordinary mesh path, and the cubemap domain (Skybox) wraps its
        // cube onto a ray-traced ball, with its own refusals about the cube rather than the domain (no
        // TextureCube in the schema / nothing bound / a dangling skybox).
        //
        // A SURFACE material only appears if its shader draws MESH GEOMETRY. The engine's one Terrain-domain
        // shader synthesizes its geometry from `gl_VertexIndex` instead — Terrain.shader as a control-point
        // patch grid for the tessellator — and has nothing that could be fed by a primitive's vertex
        // buffer. (There were two until Г25 removed Grass.shader with the procedural grass generator.) Nothing
        // errors and nothing crashes: the pane simply renders an empty scene, and a grey rectangle that explains
        // nothing is the silent fallback the delivery contract forbids (§1.4). An artist cannot tell "this domain
        // has no preview shape" from "the preview is broken", and the difference decides whether they go looking
        // for a bug.
        //
        // Also answers for the two states that are not about the domain at all — a shader that is not
        // loaded, and one that is registered but has no compiled stages (ShaderService keeps the NAME
        // either way, deliberately, and MeshRenderer skips the draw) — because an empty pane means the
        // same thing to the eye in all three cases and something different in each.
        [[nodiscard]] std::string PreviewUnavailableReason( const std::string& shaderName ) const;

        // The sentence that stays UNDER whatever fills the pane, or empty when there is nothing to
        // qualify. It is the other half of PreviewUnavailableReason and not a replacement for it: a
        // domain can now be drawn AND still need a caveat, which the Volume domain is the first to be.
        // Being told "there is no preview" and being shown a preview that means something narrower than
        // it looks are different states, and only one of them used to exist.
        [[nodiscard]] std::string PreviewSceneNote() const;

        // The second tab: the SCENE the material is shown in — sky, sun, floor, and the dome's own
        // tracing budget. It edits PreviewViewport::Setup() in place, which is why it has no Apply and
        // no push: the widget reads that struct every frame it records.
        void DrawPreviewSceneTab();

        // The pane when there is no image: the same rectangle as before, with the reason written inside
        // it. Inside, not underneath — the message has to be where the picture would have been, or it is
        // one more line in a column of labels.
        void DrawPreviewPlaceholder( float side, const std::string& reason ) const;

        // The subject's domain, or nullopt while its shader is not loaded. The panel's per-domain
        // decisions — which draw fills the pane (see OnPreUpdate), whether the Shape combo means
        // anything (Surface only), which shaders the picker lists — all read THIS, so they cannot
        // each resolve the shader and answer differently within one frame.
        [[nodiscard]] std::optional<::Desert::Core::Formats::ShaderDomain> EffectiveDomain() const;

        // The cube the subject material's cubemap slot currently resolves to, or null (no slot in the
        // schema, nothing bound, or a dangling handle — PreviewUnavailableReason tells those apart, this
        // only answers "what would the ball show"). Called from the preview pass EVERY frame via the
        // closure SetCubemapMaterial carries, which is what makes a texture drop show without any
        // invalidation call; see EditorCubemapPreviewPass.hpp for why the pass holds no copy.
        [[nodiscard]] const Graphic::ImageCube* ResolveSubjectCubemap() const;

        // The name of the shader this document's material actually draws with, or empty if the material is
        // gone. Recomputed rather than cached: the shader a material names is editable (this window's own
        // picker, and the Node Graph rewriting its scratch material), so a cached copy would leave the
        // window watching rebuilds of a shader it no longer uses.
        //
        // An INSTANCE resolves through its parent, because an instance has no shader of its own — that is
        // why it is shown no picker. Reading the child's own name gave "StaticMeshPBR", the default a
        // material with no name reports, so an instance window watched rebuilds of a shader it does not
        // draw with and kept its pipelines from before the parent shader's recompile.
        [[nodiscard]] std::string EffectiveShaderName() const;

        std::shared_ptr<Assets::AssetManager> m_AssetManager;

        // ── The three states ───────────────────────────────────────────────────────────────────────────
        //
        // WORKING lives in m_WorkingCopy->Data(). APPLIED lives in the subject asset's own Data(), which
        // is what the runtime materials of every open scene were built from. ON DISK is the snapshot
        // below. Only the third is stored here, and deliberately: the first two are already objects the
        // renderer reads, and a second copy of either would be the editor holding its own opinion of a
        // value something else owns — one source of truth per value, and the shape of defect this engine
        // has paid for repeatedly.
        std::shared_ptr<Assets::SurfaceMaterialAsset> m_WorkingCopy;

        // What the `.demat` held when this document opened, refreshed by every successful Save. The third
        // state, and the only one nothing else in the process is holding.
        //
        // Snapshotted rather than re-read from disk on demand: the question "does this differ from the
        // file" is asked every frame (it decides the document's dirty mark), and answering it with a file
        // read would put the disk in the frame loop.
        Assets::MaterialData m_OnDisk;

        // Why this document could not make its working copy, or empty while it could. Written once, shown
        // in the window, and while it is set the parameter table is READ-ONLY — editing the subject
        // directly instead would be the silent restoration of exactly the behaviour this document exists
        // to prevent, and the artist would have no way to know which of the two modes they were in.
        std::string m_WorkingCopyRefusal;

        // WHY THE LAST DROP DID NOTHING, and on which row it was aimed. One at a time, because one drop
        // happens at a time; cleared by the next successful bind on that row. Empty is the normal state.
        //
        // It exists because a drop this window could not use USED TO BE INVISIBLE — no bind, no log, no
        // message, and an artist who reads that as a broken editor rather than as a wrong file. Held here
        // rather than in a toast because the answer belongs beside the slot it is about.
        struct DropRefusal
        {
            std::string Param;   // the schema parameter name of the row the drop landed on
            std::string Message; // one sentence, from WhyThatCannotGoInThisSlot
        };
        DropRefusal m_DropRefusal;

        // Null whenever the window is not drawing — this IS the zero-cost mechanism, not an optimisation on
        // top of one. unique_ptr rather than a value member for exactly that reason.
        std::unique_ptr<PreviewViewport> m_Preview;
        std::unique_ptr<UI::UIHelper>    m_UIHelper;

        PreviewViewport::Shape m_Shape = PreviewViewport::Shape::Sphere;

        // An ARBITRARY MESH to show the material on instead of a primitive, and its filename for the row
        // that offers it. Null means "use the shape". PreviewViewport::SetMesh already framed a foreign
        // mesh off its own vertices long before this window existed; all that was missing was a caller.
        //
        // Both are part of the pushed identity below, so choosing one announces itself exactly as a shape
        // change does — there is no separate "the mesh changed" flag to forget.
        Assets::AssetHandle m_PreviewMesh{ static_cast<uint64_t>( 0 ) };
        std::string         m_PreviewMeshName;

        // WHAT WAS PUSHED, not whether something was. This used to be `bool m_Applied`, reset by hand at
        // every site that invalidates the push, and the site that changes the material's SHADER carries no
        // such reset.
        //
        // THIS IS HARDENING, NOT A BUG FIX, AND THE DISTINCTION IS THE POINT. The missing reset looks like
        // a live defect and is not one: DrawShaderPicker's call site calls MaterialService::Invalidate,
        // which bumps m_InvalidationVersion, and MeshECSSystem rebuilds the runtime instance from the new
        // shader on its next tick (Components.hpp SeenMaterialsVersion, MeshECSSystem.hpp:129). The preview
        // updates through that path, never through SetMaterial. Verified by rendering the shader switch,
        // not by reading — the reading said otherwise and the reading was wrong.
        //
        // What is real is the coupling: preview correctness depends on EVERY shader-change path calling
        // Invalidate, and nothing enforces that. Recording the identity that WAS pushed makes the re-push
        // condition DERIVED, so a future path that changes subject, shape or shader announces itself by
        // differing rather than by being remembered. Empty shader name means "nothing pushed yet".
        // Costs nothing per frame: EffectiveShaderName() is already resolved in the same function for the
        // rebuild counter below.
        struct PushedIdentity
        {
            Common::AssetHandle    Subject;
            PreviewViewport::Shape Shape = PreviewViewport::Shape::Sphere;
            std::string            ShaderName;
            // The arbitrary preview mesh, or 0 for "the shape". A fourth term rather than a second
            // condition beside the comparison, which is the whole reason this is an identity: the
            // re-push is DERIVED, so the term added today needs no new reset site anywhere.
            Common::AssetHandle Mesh{ static_cast<uint64_t>( 0 ) };

            bool operator==( const PushedIdentity& other ) const
            {
                return Subject == other.Subject && Shape == other.Shape && ShaderName == other.ShaderName &&
                       Mesh == other.Mesh;
            }
        };
        PushedIdentity m_Pushed;

        // The rebuild count this window has already acted on; see MaterialShaderRebuild for why it is a
        // count each window compares against rather than a pending value one of them consumes.
        uint64_t m_SeenRebuildCount = 0;

        // Set in OnUIRender, consumed in OnPreUpdate: the render is only paid for while the window really
        // drew last frame, so a hidden dock tab costs nothing even before the window is closed outright.
        bool m_DrewThisFrame = false;

        // PreviewUnavailableReason for the material as it stood on the last drawn frame; empty means the
        // pane shows a real render. Computed in OnUIRender and read by OnPreUpdate on the next frame —
        // the same one-frame handshake m_DrewThisFrame uses, and for the same reason: OnPreUpdate is the
        // only place allowed to build or destroy the renderer, and it must not resolve the shader a
        // second time and risk answering differently from the message already on screen.
        std::string m_PreviewUnavailable;
    };
} // namespace Desert::Editor
