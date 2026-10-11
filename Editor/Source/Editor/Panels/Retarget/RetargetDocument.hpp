#pragma once

#include <Editor/Core/SubjectEditorRegistry.hpp>
#include <Editor/Panels/IPanel.hpp>
#include <Editor/Panels/Retarget/RetargetDocumentModel.hpp>

#include <Engine/Animation/Pose.hpp>
#include <Engine/Assets/Common.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Desert::Assets
{
    class AssetManager;
    class AnimationAsset;
    class SkeletonAsset;
    class SkinnedMeshAsset;
} // namespace Desert::Assets

namespace Desert::Animation::Retarget
{
    class RetargetSource;
}

namespace Desert::Editor::UI
{
    class UIHelper;
}

namespace Desert::Editor
{
    class PreviewViewport;

    /**
     * @brief One window per `.retarget` (UE: the IK Retargeter editor): the source rig playing a clip beside the
     *        target rig posed by the retarget, the chain table with each chain's verdict, an inspector for the
     *        selected chain, Auto-map and Save.
     *
     * EVERY EDIT GOES THROUGH RetargetDocumentModel, so the suite (RetargetDocument) exercises the edit the user
     * makes. The window owns only what a device needs: the two PreviewViewports and the per-frame retarget.
     *
     * THE TARGET'S POSE IS THE RUNTIME'S. The right pane is driven by `RetargetSource::Run` — the call the
     * AnimationECSSystem makes for an entity with this retarget — rebuilt from the model whenever its revision
     * moves, so what the window shows is what the game would play, and a chain the runtime refuses shows as
     * the target standing at rest with the reason printed above it.
     *
     * ROTATION AND TRANSLATION ARE SHOWN, NOT EDITED. The runtime has one mode of each (interpolated FK along
     * the chain; the pelvis translation scaled by the rigs' height ratio): a dial for a mode nobody reads
     * would be a dead setting. The IK goal is the chain's end bone (Retargeter forbids a named goal).
     *
     * A renderer-slot claimant from birth (two previews = two SceneRenderers; the default ViewForecastBytes
     * already covers the window's area, which is what the two halves together draw).
     */
    class RetargetDocument final : public ISubjectDocument
    {
    public:
        RetargetDocument( const Assets::AssetHandle& retarget, Assets::AssetManager* assets );
        ~RetargetDocument() override;

        [[nodiscard]] glm::vec2 GetDefaultSize() const override
        {
            return { 1400.0f, 820.0f };
        }

        void OnPreUpdate() override;
        void OnUIRender() override;

        [[nodiscard]] bool IsSubjectAlive() const override;

        [[nodiscard]] bool ClaimsView() const override
        {
            return true;
        }
        [[nodiscard]] bool HoldsView() const override
        {
            return m_SourcePreview != nullptr || m_TargetPreview != nullptr;
        }
        void ReleaseView() override;

        [[nodiscard]] DiskState GetDiskState() const override;
        bool                    SaveDocument() override;
        bool                    DiscardEdits() override;

        // "Auto-map", "Save", "Play", "Pause": the toolbar from the palette, for a headless check.
        [[nodiscard]] std::vector<DocumentAction> Actions() override;

    private:
        struct Side
        {
            std::shared_ptr<Assets::SkeletonAsset>    Skeleton;
            std::shared_ptr<Assets::SkinnedMeshAsset> Mesh;
            std::string                               Problem; // why this side has no rig or no mesh
        };

        void LoadModel();
        void LoadSides();
        void EnsurePreviews();
        void SelectClip( const std::filesystem::path& path );
        void RebuildRetarget();
        void RecomputeVerdicts();
        void Pose();

        void RunAutoMap();
        void Save();

        void DrawToolbar();
        void DrawChainTable();
        void DrawInspector();
        bool BoneCombo( const char* label, const Animation::Skeleton* skeleton, std::string& bone );

        [[nodiscard]] std::filesystem::path FilePath() const;

        Assets::AssetManager*                  m_Assets = nullptr;
        std::unique_ptr<RetargetDocumentModel> m_Model;
        std::string                            m_Unavailable; // why there is no document, in the pane's words

        Side m_Source;
        Side m_Target;

        std::vector<std::filesystem::path>      m_Clips; // the source rig's clips, by path
        std::shared_ptr<Assets::AnimationAsset> m_Clip;
        std::string                             m_ClipName;
        double                                  m_Time    = 0.0;
        bool                                    m_Playing = true;

        std::unique_ptr<PreviewViewport>                     m_SourcePreview;
        std::unique_ptr<PreviewViewport>                     m_TargetPreview;
        std::unique_ptr<UI::UIHelper>                        m_SourceUI;
        std::unique_ptr<UI::UIHelper>                        m_TargetUI;
        std::unique_ptr<Animation::Retarget::RetargetSource> m_Retarget;
        Animation::LocalPose                                 m_TargetRest;
        Animation::LocalPose                                 m_TargetPose;
        std::string                                          m_RetargetError;
        std::optional<uint64_t>                              m_BuiltRevision;
        std::optional<uint64_t>                              m_VerdictRevision;
        std::vector<std::string>                             m_ChainProblems;
        std::string                                          m_Verdict; // Validate's error, empty = ok

        bool        m_DrewThisFrame = false;
        glm::uvec2  m_RenderSize{ 0u, 0u }; // one pane; both halves are this size
        int         m_Selected = -1;
        std::string m_Status; // the last Save / Auto-map outcome
        bool        m_StatusFailed = false;
    };

    // The `.retarget` path opener: find-or-register the RetargetAsset, then open it through the one handle route,
    // Core::RequestOpenAsset. Not loaded here: a file the loader refuses still opens, and the window names why.
    [[nodiscard]] SubjectEditorRegistry::PathOpenOutcome
    RequestRetargetDocument( Assets::AssetManager* assets, const std::string& path,
                             const SubjectEditorRegistry& editors );
} // namespace Desert::Editor
