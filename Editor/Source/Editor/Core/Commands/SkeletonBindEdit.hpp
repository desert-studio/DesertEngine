#pragma once

#include <Editor/Core/CommandHistory.hpp>

#include <Engine/Assets/Mesh/SkeletonAsset.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Desert::Editor
{
    /**
     * THE REFERENCE POSE OF A SKELETON IS AUTHORED IN ITS EDITOR (UE: the Skeleton Editor's bone transform edits
     * the reference pose; Save writes the USkeleton package). The Skeleton mode of the Animation Editor writes a
     * bone's LocalBindTransform through Assets::SkeletonAsset::SetLocalBindTransform, one undo record per gesture
     * (a committed Details row, or a gizmo drag from press to release), and Save writes the `.skeleton`
     * (Serialization::SaveSkeletonAsset). Every reader of the rig holds the asset's one Skeleton object, so the
     * edit is live for every mesh on it; an Animator re-reads the rest pose when GetBindRevision moves.
     *
     * Kept out of AnimationEditorDocument.cpp so a suite can compile it (SkeletonBindEdit).
     */

    /// One bone's Reference Pose, old -> new, on one skeleton. Holds the ASSET, not a pointer into its bones: a
    /// reload rebuilds the rig in place and the asset outlives the window's edits either way.
    class SkeletonBindCommand final : public ICommand
    {
    public:
        SkeletonBindCommand( std::shared_ptr<Assets::SkeletonAsset> skeleton, uint32_t bone, const glm::mat4& before,
                             const glm::mat4& after );

        bool Undo() override;
        bool Redo() override;

        [[nodiscard]] const void* EditedObject() const override
        {
            return m_Skeleton.get();
        }
        [[nodiscard]] std::string GetLabel() const override
        {
            return "Reference Pose";
        }

    private:
        std::shared_ptr<Assets::SkeletonAsset> m_Skeleton;
        uint32_t                               m_Bone;
        glm::mat4                              m_Before;
        glm::mat4                              m_After;
    };

    /// A committed field edit (the Details rows): writes @p localBind and pushes ONE record. False when the value
    /// is the one already there (no record), or the rig refuses the write (not loaded, bone out of range).
    bool CommitBindEdit( const std::shared_ptr<Assets::SkeletonAsset>& skeleton, uint32_t bone,
                         const glm::mat4& localBind );

    /// A gizmo drag on the rest pose: the press captures the bone's bind, every moved frame writes it live, the
    /// release pushes ONE record (old -> current) if anything changed - BoneGizmoGesture's shape for the bind.
    class BindPoseGesture
    {
    public:
        /// Call every frame the gizmo is drawn. @p held: the manipulator is held this frame; @p moved: the bone's
        /// new local transform this frame, or null. A change of bone or skeleton mid-drag closes the drag first.
        void Step( const std::shared_ptr<Assets::SkeletonAsset>& skeleton, uint32_t bone, bool held,
                   const glm::mat4* moved );
        /// Closes an open drag as a release would (the gizmo is no longer drawn).
        void End();

        [[nodiscard]] bool Active() const
        {
            return m_Skeleton != nullptr;
        }

    private:
        std::shared_ptr<Assets::SkeletonAsset> m_Skeleton;
        uint32_t                               m_Bone = 0;
        glm::mat4                              m_Before{ 1.0f };
    };

    /// Every bone's LocalBindTransform as the `.skeleton` file states it: what "Save*" compares against and what
    /// "Don't Save" puts back. Read from the file, never a snapshot of the shared asset (which may hold edits).
    [[nodiscard]] Common::ResultStr<std::vector<glm::mat4>> ReadBindPoseOnDisk( const Assets::SkeletonAsset& skeleton );

    /// True when the rig in memory has a rest pose other than @p onDisk (or another bone count).
    [[nodiscard]] bool BindPoseDiffers( const Assets::SkeletonAsset& skeleton, std::span<const glm::mat4> onDisk );

    /// "Don't Save": @p onDisk back into the rig, and every record of this skeleton forgotten (they would redo an
    /// edit the user threw away). False when the rig is not loaded or the counts differ.
    bool RestoreBindPose( const std::shared_ptr<Assets::SkeletonAsset>& skeleton, std::span<const glm::mat4> onDisk );
} // namespace Desert::Editor
