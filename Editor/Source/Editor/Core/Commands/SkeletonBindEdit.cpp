#include "SkeletonBindEdit.hpp"

#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/Serialization/Skeleton.hpp>

namespace Desert::Editor
{
    SkeletonBindCommand::SkeletonBindCommand( std::shared_ptr<Assets::SkeletonAsset> skeleton, const uint32_t bone,
                                              const glm::mat4& before, const glm::mat4& after )
         : m_Skeleton( std::move( skeleton ) ), m_Bone( bone ), m_Before( before ), m_After( after )
    {
    }

    bool SkeletonBindCommand::Undo()
    {
        return m_Skeleton && m_Skeleton->SetLocalBindTransform( m_Bone, m_Before );
    }

    bool SkeletonBindCommand::Redo()
    {
        return m_Skeleton && m_Skeleton->SetLocalBindTransform( m_Bone, m_After );
    }

    namespace
    {
        const glm::mat4* BindOf( const Assets::SkeletonAsset& skeleton, const uint32_t bone )
        {
            const Animation::Skeleton* rig = skeleton.GetSkeleton();
            if ( rig == nullptr || bone >= rig->GetBones().size() )
                return nullptr;
            return &rig->GetBones()[bone].LocalBindTransform;
        }
    } // namespace

    bool CommitBindEdit( const std::shared_ptr<Assets::SkeletonAsset>& skeleton, const uint32_t bone,
                         const glm::mat4& localBind )
    {
        if ( !skeleton )
            return false;
        const glm::mat4* current = BindOf( *skeleton, bone );
        if ( current == nullptr || *current == localBind )
            return false;
        const glm::mat4 before = *current;
        if ( !skeleton->SetLocalBindTransform( bone, localBind ) )
            return false;
        CommandHistory::Get().PushCommand( std::make_unique<SkeletonBindCommand>( skeleton, bone, before, localBind ) );
        return true;
    }

    void BindPoseGesture::Step( const std::shared_ptr<Assets::SkeletonAsset>& skeleton, const uint32_t bone,
                                const bool held, const glm::mat4* moved )
    {
        if ( Active() && ( m_Skeleton != skeleton || m_Bone != bone ) )
            End();
        if ( held && !Active() && skeleton )
        {
            if ( const glm::mat4* current = BindOf( *skeleton, bone ) )
            {
                m_Skeleton = skeleton;
                m_Bone     = bone;
                m_Before   = *current;
            }
        }
        if ( Active() && moved != nullptr )
            (void)m_Skeleton->SetLocalBindTransform( m_Bone, *moved );
        if ( !held )
            End();
    }

    void BindPoseGesture::End()
    {
        if ( !Active() )
            return;
        const std::shared_ptr<Assets::SkeletonAsset> skeleton = std::move( m_Skeleton );
        m_Skeleton.reset();
        const glm::mat4* current = BindOf( *skeleton, m_Bone );
        if ( current != nullptr && *current != m_Before )
            CommandHistory::Get().PushCommand(
                 std::make_unique<SkeletonBindCommand>( skeleton, m_Bone, m_Before, *current ) );
    }

    Common::ResultStr<std::vector<glm::mat4>> ReadBindPoseOnDisk( const Assets::SkeletonAsset& skeleton )
    {
        auto read = Assets::Serialization::ReadSkeletonFile(
             Assets::ContentRegistry::FileToOpen( skeleton.GetMetadata().Filepath ) );
        if ( !read )
            return Common::MakeError<std::vector<glm::mat4>>( read.GetError() );
        std::vector<glm::mat4> binds;
        binds.reserve( read.GetValue().Bones.size() );
        for ( const auto& bone : read.GetValue().Bones )
            binds.push_back( bone.LocalBindTransform );
        return Common::MakeSuccess( std::move( binds ) );
    }

    bool BindPoseDiffers( const Assets::SkeletonAsset& skeleton, const std::span<const glm::mat4> onDisk )
    {
        const Animation::Skeleton* rig = skeleton.GetSkeleton();
        if ( rig == nullptr )
            return false;
        const auto& bones = rig->GetBones();
        if ( bones.size() != onDisk.size() )
            return true;
        for ( size_t i = 0; i < bones.size(); ++i )
            if ( bones[i].LocalBindTransform != onDisk[i] )
                return true;
        return false;
    }

    bool RestoreBindPose( const std::shared_ptr<Assets::SkeletonAsset>& skeleton, const std::span<const glm::mat4> onDisk )
    {
        if ( !skeleton || skeleton->GetSkeleton() == nullptr ||
             skeleton->GetSkeleton()->GetBones().size() != onDisk.size() )
            return false;
        for ( uint32_t i = 0; i < onDisk.size(); ++i )
            if ( skeleton->GetSkeleton()->GetBones()[i].LocalBindTransform != onDisk[i] )
                (void)skeleton->SetLocalBindTransform( i, onDisk[i] );
        CommandHistory::Get().DropFor( skeleton.get() );
        return true;
    }
} // namespace Desert::Editor
