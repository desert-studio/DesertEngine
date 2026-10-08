#pragma once

#include <Editor/Core/CommandHistory.hpp>

#include <Engine/Physics/PhysicsAssetFormat.hpp>
#include <Engine/Physics/PhysicsWorld.hpp>
#include <Engine/Physics/RagdollDesc.hpp>

#include <Common/Core/Core.hpp>
#include <Common/Core/ResultStr.hpp>

#include <memory>
#include <string>
#include <vector>

namespace Desert::Animation
{
    class Skeleton;
    class LocalPose;
    struct BoneOverride;
} // namespace Desert::Animation

namespace Desert::Editor
{
    /**
     * THE PHYSICS ASSET EDITOR'S RENDERER-FREE HALF (UE's Physics Asset Editor, PhAT): the edits, the save and
     * the Simulate preview. Kept out of PhysicsAssetEditorDocument.cpp so a suite can compile it without a device.
     *
     * THE DOCUMENT OWNS ONE WORKING COPY of the asset's data (shared, so an undo record outlives the window that
     * pushed it). Every committed field edit replaces the copy and pushes ONE record holding the whole asset
     * before and after — a physics asset is a few dozen bodies, so a whole-copy record is the simplest record
     * that cannot desynchronise from what it restores. Save writes the copy through Assets::PhysicsAsset::Save,
     * the asset serializer every reader decodes.
     */
    class PhysicsAssetEditCommand final : public ICommand
    {
    public:
        PhysicsAssetEditCommand( std::shared_ptr<Physics::PhysicsAssetData> target,
                                 Physics::PhysicsAssetData before, Physics::PhysicsAssetData after,
                                 std::string label );

        bool Undo() override;
        bool Redo() override;

        [[nodiscard]] const void* EditedObject() const override
        {
            return m_Target.get();
        }
        [[nodiscard]] std::string GetLabel() const override
        {
            return m_Label;
        }

    private:
        std::shared_ptr<Physics::PhysicsAssetData> m_Target;
        Physics::PhysicsAssetData                  m_Before;
        Physics::PhysicsAssetData                  m_After;
        std::string                                m_Label;
    };

    /// Replaces @p target's data with @p after and pushes ONE record. False (no record) when nothing changed.
    bool CommitPhysicsAssetEdit( const std::shared_ptr<Physics::PhysicsAssetData>& target,
                                 Physics::PhysicsAssetData after, const std::string& label );

    /// The joint whose child is @p childBone gets the three limits (degrees, each in [0, 180]); one record.
    Common::BoolResultStr CommitJointLimits( const std::shared_ptr<Physics::PhysicsAssetData>& target,
                                             const std::string& childBone, float swing1Degrees,
                                             float swing2Degrees, float twistDegrees );

    /// The body of @p body.Bone becomes @p body (shape, size, transform, mass); one record. Refuses a bone with
    /// no body and a negative size.
    Common::BoolResultStr CommitBody( const std::shared_ptr<Physics::PhysicsAssetData>& target,
                                      const Physics::PhysicsAssetBody&                  body );

    /// Writes @p data to @p path through the asset serializer (the file's GUID is kept).
    Common::BoolResultStr SavePhysicsAssetDocument( const Common::Filepath&          path,
                                                    const Physics::PhysicsAssetData& data );

    /**
     * SIMULATE (UE PhAT's Simulate button): the ragdoll runs in a PhysicsWorld this object creates and owns —
     * never the scene's, so pressing Simulate in an asset window cannot touch the level, and the level's play
     * cannot touch the preview. A floor with its top face at y = 0 (the preview's floor) catches it. Stop
     * destroys the world; the window then puts the animated pose back, so every Start begins from the pose.
     */
    class PhysicsAssetPreviewSimulation
    {
    public:
        PhysicsAssetPreviewSimulation();
        ~PhysicsAssetPreviewSimulation();

        /// Builds the ragdoll of @p asset on @p skeleton, posed as @p pose, in a new private world.
        Common::BoolResultStr Start( const Physics::PhysicsAssetData& asset, const Animation::Skeleton& skeleton,
                                     const Animation::LocalPose& pose );
        void                  Step( float dt );
        void                  Stop();

        [[nodiscard]] bool IsRunning() const
        {
            return m_World != nullptr;
        }

        /// The simulated bodies as component-space bone overrides over @p animated (Animator::ApplyPhysicsPose).
        [[nodiscard]] Common::ResultStr<std::vector<Animation::BoneOverride>>
        Overrides( const Animation::Skeleton& skeleton, const Animation::LocalPose& animated );

        /// Every part's world transform after the last Start / Step (resolve order, as the description's parts):
        /// what the window draws the bodies at while simulating. Empty when stopped.
        [[nodiscard]] const std::vector<Physics::RagdollPartTransform>& Parts() const
        {
            return m_Parts;
        }
        [[nodiscard]] const Physics::RagdollDesc& Description() const
        {
            return m_Desc;
        }

        /// The private world (null when stopped); exposed for the suite that proves it is not the scene's.
        [[nodiscard]] const Physics::PhysicsWorld* World() const
        {
            return m_World.get();
        }

    private:
        std::unique_ptr<Physics::PhysicsWorld>     m_World;
        Physics::RagdollDesc                       m_Desc;
        Physics::RagdollHandle                     m_Ragdoll = Physics::kInvalidRagdoll;
        std::vector<Physics::RagdollPartTransform> m_Parts;
    };
} // namespace Desert::Editor
