#pragma once

#include <Editor/Core/Commands/PoseEditTransaction.hpp>
#include <Editor/Panels/AnimationEditor/AnimationEditorIdentity.hpp>
#include <Editor/Panels/AnimationEditor/AnimationNotifyTracks.hpp>
#include <Editor/Panels/AnimationEditor/AnimationTransport.hpp>
#include <Editor/Widgets/MeshAssetDetails.hpp>

#include <Common/Core/Core.hpp> // Common::Filepath, which AssetMetadata.hpp names without including
#include <Engine/Assets/AssetMetadata.hpp>
#include <Engine/Assets/AssetRootPin.hpp>

#include <Engine/Animation/AnimationClip.hpp>

#include <glm/glm.hpp>

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Desert::Assets
{
    class AssetManager;
    class AnimationAsset;
    class SkinnedMeshAsset;
} // namespace Desert::Assets

namespace Desert::Editor::UI
{
    class UIHelper;
}

namespace Desert::Editor
{
    class PreviewViewport;

    /**
     * @brief One window per animation clip (`.anim`): UE's Animation Editor (Persona) — part 1, the preview
     * viewport and the transport.
     *
     * THE MESH IS FOUND BY THE CLIP'S RIG. A clip names a skeleton signature and nothing else; the window shows
     * the first registered `.skmesh` (by path, so the pick is stable) whose skeleton signature is the clip's.
     * No such mesh is a named error in the window — the signature by number — never a stand-in mesh.
     *
     * THE POSE IS A FUNCTION OF THE TRANSPORT'S TIME. AnimationTransport owns the clock; the preview's animator
     * is stopped and told the time every frame (PreviewViewport::SetAnimationTime), so a scrub, a step or a
     * palette "Set Time" land on the same pose a playing clip shows at that time.
     *
     * THE BOTTOM PANEL IS UE's TIMELINE: a ruler in display-rate frames (Sequencer::TimeAxis/DrawFrameGrid, the
     * Sequencer's own), the playhead on the transport's time (a drag on the ruler scrubs), the Notifies tracks
     * and a "Curves (0)" header — the clip has no float curves yet. Every notify edit goes through
     * ApplyNotifyEdit (AnimationNotifyTracks.hpp): one undo record per add / move / rename / delete, a drag
     * committed on release. Save writes the `.anim` through SaveClipToFile; the clip is dirty while its
     * notifies differ from the ones last read from or written to the file.
     *
     * THE LAYOUT IS UE's PERSONA, in the window's own DockSpace: Asset Details | Skeleton Tree tabs on the left,
     * the viewport with the transport and timeline in the middle, Details (the selected bone) | Preview Scene
     * Settings on the right. The layout is built once, when the dock node does not exist yet; after that ImGui's
     * ini keeps whatever the person made of it. The selected bone is THIS WINDOW's (a preview is not the
     * scene, so AuthoringContext's bone selection is not touched); it is lit over the picture with its name,
     * and Show Bones draws every bone as a line to its parent.
     */
    class AnimationEditorDocument final : public AnimationEditorBase
    {
    public:
        // @p asset is the clip, skeletal mesh or skeleton @p mode is about (Core::PersonaModeFor). @p editors
        // opens the clip picked in the Asset Browser and the asset a mode button names.
        AnimationEditorDocument( const Assets::AssetHandle& asset, Core::PersonaMode mode,
                                 Assets::AssetManager* assets, const SubjectEditorRegistry* editors );
        ~AnimationEditorDocument() override;

        [[nodiscard]] glm::vec2 GetDefaultSize() const override
        {
            return { 1200.0f, 800.0f };
        }

        void OnPreUpdate() override;
        void OnUIRender() override;

        [[nodiscard]] bool IsSubjectAlive() const override;

        [[nodiscard]] bool HasPreview() const override
        {
            return true;
        }
        // UE: an asset editor is a major tab of its own, not a tab beside the level viewport.
        [[nodiscard]] bool OpensAsMajorTab() const override
        {
            return true;
        }
        void SetPreviewViewpoint( const PreviewViewpoint& viewpoint ) override;

        // Palette entries for a headless shot: "Play/Pause", "Set Time <p>%", "Next Frame", "Previous Frame",
        // "Add Notify Track", "Add Notify at <p>%" (on the last track, named "Notify <n>"), "Save". <p> is a
        // tenth or a quarter of the clip: a palette label carries no argument, so the grid is enumerated.
        [[nodiscard]] std::vector<DocumentAction> Actions() override;

        [[nodiscard]] DiskState GetDiskState() const override;
        bool                    SaveDocument() override;
        // Puts the file's notifies and curves back into the shared clip ("Don't Save" on close).
        bool DiscardEdits() override;

    protected:
        void DestroyPreview() override;

    private:
        void EnsurePreview();
        // The rig and the preferred preview mesh of the subject, by mode; false with m_Unavailable set.
        bool ResolveRig( const std::string& name, std::filesystem::path& preferredMesh );
        // UE's mode switcher (Skeleton | Mesh | Animation): opens-or-focuses the asset the mode is about.
        void                                 DrawModeToolbar();
        [[nodiscard]] Assets::AssetHandle    ModeAsset( Core::PersonaMode mode ) const;
        void                                 OpenMode( Core::PersonaMode mode );
        void                                 DrawMeshDetails();
        void                                 DrawSkeletonDetails();
        void                                 DrawOverlay( const glm::vec2& origin ) const;
        void                                 BuildLayout( unsigned int dockId ) const;
        void                                 DrawViewportPanel();
        void                                 DrawSkeletonTree();
        void                                 DrawBoneDetails();
        void                                 DrawBoneGizmo( const glm::vec2& origin, const glm::vec2& size );
        [[nodiscard]] Animation::FrameNumber KeyTick() const;
        Animation::Animator*                 BeginPosing();
        void                                 EndPosing();
        bool                      PoseSelectedBone( const Animation::BoneTransform& pose, bool undoable );
        bool                      KeySelectedBone();
        void                      DrawAssetDetails();
        void                      DrawPreviewSceneSettings();
        void                      DrawAssetBrowser();
        void                      DrawBones( const glm::vec2& origin, const glm::vec2& size ) const;
        void                      SetPreviewMesh( size_t candidate );
        [[nodiscard]] std::string PanelTitle( const char* name ) const;
        void                      DrawTransport();
        void                      DrawTimeline( float width, float height );
        void                      DrawNotifyPopups( Animation::AnimationClip& clip );
        void                      DrawCurvePopups( Animation::AnimationClip& clip );
        bool                      EditCurveKey( const std::string& name, int32_t tick, float value );
        [[nodiscard]] float       TimelineHeight() const;

        // The clip asset, found and loaded on first use (a palette entry can run before the first draw).
        [[nodiscard]] Assets::AnimationAsset* ClipAsset();
        bool EditNotifies( std::vector<Animation::AnimationNotify> edited, std::string label );
        bool AddNotify( std::string name, double seconds, int32_t track, int32_t durationTicks = 0 );

        Assets::AssetManager*            m_Assets  = nullptr;
        const SubjectEditorRegistry*     m_Editors = nullptr;
        std::unique_ptr<PreviewViewport> m_Preview;
        std::unique_ptr<UI::UIHelper>    m_UIHelper;
        bool                             m_DrewThisFrame = false;
        // Frames until the Skeleton Tree and the bone's Details are brought to the front of their dock nodes.
        // A dock node's SelectedTabId set by the builder loses to the window focused LAST on creation (the
        // Preview Scene Settings tab), so the front tabs are focused once the windows exist — on every open,
        // which is what makes a reopened editor come back with the same tabs in front.
        int                        m_FrontTabsFrames = 2;
        glm::uvec2                 m_RenderSize{ 0u, 0u };
        std::string                m_Unavailable; // why there is no picture, in the words the pane shows
        std::string                m_ClipName;
        std::string                m_MeshName;
        AnimationTransport         m_Transport;
        std::unique_ptr<glm::vec2> m_PendingOrbitDegrees;

        // The clip is a sweep root while this window is open: unsaved notify edits live only in its payload.
        Assets::AssetRootPin                    m_ClipPin;
        std::shared_ptr<Assets::AnimationAsset> m_ClipAsset;
        std::filesystem::path                   m_ClipPath;
        bool m_Tracked = false; // the clip came from a file, which m_OnDisk holds as read or last written
        // What the FILE holds, read from it on open (never a snapshot of the shared asset, which outlives the
        // window with its edits) — the "Save*" rule is ClipDiffersFromFile against it.
        std::optional<Animation::AnimationClip> m_OnDisk;
        std::string                             m_SaveStatus;
        bool                                    m_SaveFailed      = false;
        int32_t                                 m_AddedNotifyRows = 0;
        std::vector<float>                      m_Flash; // seconds of highlight left, per notify
        int32_t                                 m_DragNotify   = -1;
        int32_t                                 m_PopupNotify  = -1;
        int32_t                                 m_PopupTrack   = 0;
        double                                  m_PopupSeconds = 0.0;
        std::array<char, 128>                   m_NameBuffer{};
        // Every registered skeletal mesh on the clip's rig, sorted by path; the preview shows m_MeshIndex.
        // Candidates by the registry's Rig tag, NOT loaded: only the one shown is (ANV1c3 loaded every one).
        std::vector<std::filesystem::path>        m_MeshCandidates;
        uint64_t                                  m_Signature = 0; // the rig every mode of this window is on
        std::shared_ptr<Assets::SkinnedMeshAsset> m_Mesh;
        // The Mesh mode's skinning audit, cached against (mesh handle, bone count): a vertex scan per frame is
        // waste.
        MeshAssetDetails::SkinningAudit m_SkinningAudit;
        uint64_t                        m_SkinningAuditOf    = 0;
        std::size_t                     m_SkinningAuditBones = 0;
        // Notify State edge drag, curve key drag and the curve popups.
        int32_t         m_DragState = -1;
        NotifyStateEdge m_DragEdge  = NotifyStateEdge::End;
        int32_t         m_DragCurve = -1;
        int32_t         m_DragKey   = -1;
        std::string     m_PopupCurve;
        int32_t         m_PopupKeyTick  = 0;
        float           m_PopupKeyValue = 0.0f;
        bool            m_PopupAddState = false;
        // Asset Browser: the clips of the preview's rig, listed once per mesh.
        std::array<char, 64>                                     m_BrowserFilter{};
        std::vector<std::pair<std::string, Assets::AssetHandle>> m_BrowserClips;
        bool                                                     m_BrowserListed = false;
        size_t                                                   m_MeshIndex     = 0;
        std::optional<uint32_t>                                  m_SelectedBone;
        std::vector<bool>                                        m_CollapsedBones;
        std::array<char, 64>                                     m_BoneFilter{};
        bool                                                     m_ShowBones = false;

        // POSING (UE Persona's bone gizmo and "+ Key"). While m_Posed the preview shows the animator's
        // authoring pose instead of the clip's; it is dropped, unkeyed, when the frame changes or play starts.
        PoseEditTransaction m_PoseEdit;
        bool                m_Posed        = false;
        int32_t             m_PosedFrame   = 0;
        bool                m_GizmoRotate  = true; // E rotate / W translate, as the level viewport
        BoneGizmoGesture    m_BoneGesture;
        bool                m_GizmoHovered = false; // this frame's: the preview yields the press to the gizmo
    };

    // Persona's path opener for `.anim`, `.skmesh` and `.skeleton` (its three modes): find-or-create the asset
    // as its type, load it, then open it through the one handle route, Core::RequestOpenAsset. Any other
    // extension is NotMine.
    [[nodiscard]] SubjectEditorRegistry::PathOpenOutcome
    RequestAnimationEditorDocument( Assets::AssetManager* assets, const std::string& path,
                                    const SubjectEditorRegistry& editors );

    // The clip file extension the opener claims.
    inline constexpr const char* kAnimationClipExtension = ".anim";
} // namespace Desert::Editor
