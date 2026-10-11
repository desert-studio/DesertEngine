#pragma once

#include <Editor/Panels/PhysicsAssetEditor/PhysicsAssetEdit.hpp>
#include <Editor/Panels/PhysicsAssetEditor/PhysicsAssetEditorIdentity.hpp>

#include <Common/Core/Core.hpp> // Common::Filepath, which AssetMetadata.hpp names without including
#include <Engine/Animation/Pose.hpp>
#include <Engine/Assets/AssetMetadata.hpp>
#include <Engine/Physics/RagdollDesc.hpp>

#include <glm/glm.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Editor::UI
{
    class UIHelper;
}

namespace Desert::Editor
{
    class PreviewViewport;

    /**
     * @brief One window per physics asset (`.dephysasset`): UE's Physics Asset Editor (PhAT). The skeletal mesh
     * the asset's skeleton skins stands in a preview with every body drawn over it as a wireframe; the left pane
     * is the bone tree, the right pane the selected bone's body (shape, size, placement, mass) and its joint's
     * limits; Simulate drops the ragdoll in the preview.
     *
     * EVERY WIDGET IS A THIN CALLER of PhysicsAssetEdit (the tested renderer-free half): a committed field is
     * CommitBody / CommitJointLimits (one undo record each), Save is SavePhysicsAssetDocument, Simulate is a
     * PhysicsAssetPreviewSimulation — the private simulation that owns its own world, so nothing this window does
     * reaches the level's physics (the PhysicsAssetEditor census reads this file and its .cpp for it).
     *
     * THE PREVIEW POSE goes through the preview animator's authoring pose (PreviewViewport::SetPoseOverride, the
     * Animation Editor's posing): stopped, it is the rest pose; simulating, it is the rest pose with the simulated
     * bodies written in by Animator::ApplyPhysicsPose each frame. Stop puts the rest pose back.
     *
     * Edits are refused while simulating (UE greys the details out): the running ragdoll was built from the asset
     * as it was at Start.
     */
    class PhysicsAssetEditorDocument final : public PhysicsAssetEditorBase
    {
    public:
        PhysicsAssetEditorDocument( const Assets::AssetHandle& asset, Assets::AssetManager* assets );
        ~PhysicsAssetEditorDocument() override;

        [[nodiscard]] glm::vec2 GetDefaultSize() const override
        {
            return { 1280.0f, 800.0f };
        }

        void OnPreUpdate() override;
        void OnUIRender() override;

        [[nodiscard]] bool IsSubjectAlive() const override;
        void               ReleaseView() override;

        [[nodiscard]] bool HasPreview() const override
        {
            return true;
        }
        void SetPreviewViewpoint( const PreviewViewpoint& viewpoint ) override;

        // Writes the working copy to the `.dephysasset` and reloads the asset the runtime reads.
        bool SaveDocument() override;

        // "Simulate", "Stop", "Save": the toolbar's buttons from the palette and the control channel.
        [[nodiscard]] std::vector<DocumentAction> Actions() override;

    private:
        void LoadWorkingCopy();
        void EnsurePreview();
        void StartSimulation();
        void StopSimulation();
        void StepSimulation();
        void RebuildDescriptionIfChanged( const Animation::Skeleton& skeleton );

        void DrawToolbar();
        void DrawBoneTree();
        void DrawDetails();
        void DrawBodyDetails( const Physics::PhysicsAssetBody& current );
        void DrawJointDetails( const Physics::PhysicsAssetConstraint& current );
        void DrawBodies( const glm::vec2& origin, const glm::vec2& size );
        void Report( const Common::BoolResultStr& outcome, const char* what );

        Assets::AssetManager*            m_Assets = nullptr;
        std::filesystem::path            m_Path;
        std::unique_ptr<PreviewViewport> m_Preview;
        std::unique_ptr<UI::UIHelper>    m_UIHelper;
        bool                             m_DrewThisFrame = false;
        glm::uvec2                       m_RenderSize{ 0u, 0u };
        std::string                      m_Unavailable; // why there is no window body, in the words it shows
        std::optional<glm::vec2>         m_PendingOrbitDegrees;

        // The skeletal mesh the preview shows: the first `.skmesh` by path on the asset's skeleton.
        std::string m_MeshName;

        // The rest pose captured once the preview's animator exists; what Simulate starts from and Stop restores.
        std::optional<Animation::LocalPose> m_RestPose;

        // The bodies as the working copy describes them, rebuilt when the working copy changes (an edit, an undo).
        std::optional<Physics::PhysicsAssetData> m_DescribedData;
        Physics::RagdollDesc                     m_Description;
        std::string                              m_DescriptionError; // ValidatePhysicsAsset's messages

        PhysicsAssetPreviewSimulation m_Simulation;

        std::string m_SelectedBone;
        std::string m_LastRefusal;

        // The Details fields while a widget is being dragged: refreshed from the working copy only when no field
        // is active, so a drag accumulates and commits once (IsItemDeactivatedAfterEdit).
        bool                      m_EditingDetails = false;
        Physics::PhysicsAssetBody m_BodyEdit;
        glm::vec3                 m_BodyEulerDegrees{ 0.0f };
        glm::vec3                 m_JointLimitsDegrees{ 0.0f }; // swing1, swing2, twist
    };

    // The `.dephysasset` path opener (a Content Browser double-click, AssetOpen by path): find-or-create the
    // PhysicsAsset, load it, open it through Core::RequestOpenAsset. Any other extension is NotMine.
    [[nodiscard]] SubjectEditorRegistry::PathOpenOutcome
    RequestPhysicsAssetDocument( Assets::AssetManager* assets, const std::string& path,
                                 const SubjectEditorRegistry& editors );

    /**
     * @brief UE's "Create Physics Asset" on a skeletal mesh: Physics::GeneratePhysicsAsset over the mesh's
     * vertices and its skeleton, written next to the mesh as `<Mesh>_PhysicsAsset.dephysasset` (a free name:
     * `<Mesh>_PhysicsAsset1`, `2`, ... when it is taken — an existing asset is never overwritten). Returns the
     * written path; refuses, naming the mesh, a mesh with no skeleton, a skeleton that does not load, or a mesh
     * the generator finds no body in.
     */
    [[nodiscard]] Common::ResultStr<std::filesystem::path>
    CreatePhysicsAssetForMesh( Assets::AssetManager& assets, const std::filesystem::path& mesh );
} // namespace Desert::Editor
