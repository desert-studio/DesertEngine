#pragma once

#include <Editor/Panels/AnimationEditor/AnimationEditorIdentity.hpp>
#include <Editor/Panels/AnimationEditor/AnimationTransport.hpp>

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
}

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
        AnimationEditorDocument( const Assets::AssetHandle& clip, Assets::AssetManager* assets );
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
        void SetPreviewViewpoint( const PreviewViewpoint& viewpoint ) override;

        // Palette entries for a headless shot: "Play/Pause", "Set Time <p>%", "Next Frame", "Previous Frame",
        // "Add Notify Track", "Add Notify at <p>%" (on the last track, named "Notify <n>"), "Save". <p> is a
        // tenth or a quarter of the clip: a palette label carries no argument, so the grid is enumerated.
        [[nodiscard]] std::vector<DocumentAction> Actions() override;

        [[nodiscard]] DiskState GetDiskState() const override;
        bool                    SaveDocument() override;

    protected:
        void DestroyPreview() override;

    private:
        void EnsurePreview();
        void DrawOverlay( const glm::vec2& origin ) const;
        void                      BuildLayout( unsigned int dockId ) const;
        void                      DrawViewportPanel();
        void                      DrawSkeletonTree();
        void                      DrawBoneDetails() const;
        void                      DrawAssetDetails();
        void                      DrawPreviewSceneSettings();
        void                      DrawBones( const glm::vec2& origin, const glm::vec2& size ) const;
        void                      SetPreviewMesh( size_t candidate );
        [[nodiscard]] std::string PanelTitle( const char* name ) const;
        void DrawTransport();
        void                DrawTimeline( float width, float height );
        void                DrawNotifyPopups( Animation::AnimationClip& clip );
        [[nodiscard]] float TimelineHeight() const;

        // The clip asset, found and loaded on first use (a palette entry can run before the first draw).
        [[nodiscard]] Assets::AnimationAsset* ClipAsset();
        bool EditNotifies( std::vector<Animation::AnimationNotify> edited, std::string label );
        bool AddNotify( std::string name, double seconds, int32_t track );

        Assets::AssetManager*            m_Assets = nullptr;
        std::unique_ptr<PreviewViewport> m_Preview;
        std::unique_ptr<UI::UIHelper>    m_UIHelper;
        bool                             m_DrewThisFrame = false;
        glm::uvec2                       m_RenderSize{ 0u, 0u };
        std::string                      m_Unavailable; // why there is no picture, in the words the pane shows
        std::string                      m_ClipName;
        std::string                      m_MeshName;
        AnimationTransport               m_Transport;
        std::unique_ptr<glm::vec2>       m_PendingOrbitDegrees;

        // The clip is a sweep root while this window is open: unsaved notify edits live only in its payload.
        Assets::AssetRootPin                    m_ClipPin;
        std::shared_ptr<Assets::AnimationAsset> m_ClipAsset;
        std::filesystem::path                   m_ClipPath;
        bool                                    m_Tracked = false; // m_OnDiskNotifies is what the file holds
        std::vector<Animation::AnimationNotify> m_OnDiskNotifies;
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
        std::vector<std::pair<std::string, std::shared_ptr<Assets::SkinnedMeshAsset>>> m_MeshCandidates;
        size_t                                                                         m_MeshIndex = 0;
        std::optional<uint32_t>                                                        m_SelectedBone;
        std::vector<bool>                                                              m_CollapsedBones;
        std::array<char, 64>                                                           m_BoneFilter{};
        bool                                                                           m_ShowBones = false;
    };

    // The `.anim` path opener: find-or-create the AnimationAsset, load it, then open it through the one handle
    // route, Core::RequestOpenAsset. Any other extension is NotMine.
    [[nodiscard]] SubjectEditorRegistry::PathOpenOutcome
    RequestAnimationEditorDocument( Assets::AssetManager* assets, const std::string& path,
                                    const SubjectEditorRegistry& editors );

    // The clip file extension the opener claims.
    inline constexpr const char* kAnimationClipExtension = ".anim";
} // namespace Desert::Editor
