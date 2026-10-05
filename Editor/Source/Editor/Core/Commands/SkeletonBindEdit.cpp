#include "SkeletonBindEdit.hpp"

#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/Mesh/SkeletonReferenceAssets.hpp>
#include <Engine/Assets/Serialization/Skeleton.hpp>

#include <array>

#include <format>

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
        /// The rename's one rule, for the commit, the undo and the redo alike: the bone, then its channels in the
        /// loaded clips - the clips only once the rig took the name.
        Common::BoolResultStr RenameBoneAndClips( Assets::SkeletonAsset& skeleton, const uint32_t bone,
                                                  const std::string& from, const std::string& to,
                                                  const ResidentSkeletonClips& clips )
        {
            if ( auto renamed = skeleton.RenameBone( bone, to ); !renamed )
                return renamed;
            if ( clips.Loaded != nullptr )
            {
                const std::array<Animation::Timeline::BoneRename, 1> rename{
                     Animation::Timeline::BoneRename{ from, to } };
                (void)Assets::RenameBonesInResidentClips( clips.Skeleton, rename, *clips.Loaded );
            }
            return Common::MakeSuccess( true );
        }
    } // namespace

    SkeletonRenameCommand::SkeletonRenameCommand( std::shared_ptr<Assets::SkeletonAsset> skeleton,
                                                  const uint32_t bone, std::string before, std::string after,
                                                  ResidentSkeletonClips clips )
         : m_Skeleton( std::move( skeleton ) ), m_Bone( bone ), m_Before( std::move( before ) ),
           m_After( std::move( after ) ), m_Clips( clips )
    {
    }

    bool SkeletonRenameCommand::Undo()
    {
        return m_Skeleton && RenameBoneAndClips( *m_Skeleton, m_Bone, m_After, m_Before, m_Clips ).IsSuccess();
    }

    bool SkeletonRenameCommand::Redo()
    {
        return m_Skeleton && RenameBoneAndClips( *m_Skeleton, m_Bone, m_Before, m_After, m_Clips ).IsSuccess();
    }

    Common::BoolResultStr CommitBoneRename( const std::shared_ptr<Assets::SkeletonAsset>& skeleton,
                                            const uint32_t bone, const std::string& name,
                                            const ResidentSkeletonClips& clips )
    {
        if ( !skeleton || skeleton->GetSkeleton() == nullptr )
            return Common::MakeError<bool>( "the rig is not loaded" );
        const auto& bones = skeleton->GetSkeleton()->GetBones();
        if ( bone >= bones.size() )
            return Common::MakeFormattedError<bool>( "bone {} is out of range", bone );
        if ( bones[bone].Name == name )
            return Common::MakeSuccess( true );
        std::string before = bones[bone].Name;
        if ( auto renamed = RenameBoneAndClips( *skeleton, bone, before, name, clips ); !renamed )
            return renamed;
        CommandHistory::Get().PushCommand(
             std::make_unique<SkeletonRenameCommand>( skeleton, bone, std::move( before ), name, clips ) );
        return Common::MakeSuccess( true );
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

    Common::ResultStr<ReferencePoseOnDisk> ReadBindPoseOnDisk( const Assets::SkeletonAsset& skeleton )
    {
        auto read = Assets::Serialization::ReadSkeletonFile(
             Assets::ContentRegistry::FileToOpen( skeleton.GetMetadata().Filepath ) );
        if ( !read )
            return Common::MakeError<ReferencePoseOnDisk>( read.GetError() );
        ReferencePoseOnDisk onDisk;
        onDisk.Names.reserve( read.GetValue().Bones.size() );
        onDisk.Binds.reserve( read.GetValue().Bones.size() );
        for ( const auto& bone : read.GetValue().Bones )
        {
            onDisk.Names.push_back( bone.Name );
            onDisk.Binds.push_back( bone.LocalBindTransform );
        }
        return Common::MakeSuccess( std::move( onDisk ) );
    }

    bool BindPoseDiffers( const Assets::SkeletonAsset& skeleton, const ReferencePoseOnDisk& onDisk )
    {
        const Animation::Skeleton* rig = skeleton.GetSkeleton();
        if ( rig == nullptr )
            return false;
        const auto& bones = rig->GetBones();
        if ( bones.size() != onDisk.Binds.size() || bones.size() != onDisk.Names.size() )
            return true;
        for ( size_t i = 0; i < bones.size(); ++i )
            if ( bones[i].LocalBindTransform != onDisk.Binds[i] || bones[i].Name != onDisk.Names[i] )
                return true;
        return false;
    }

    bool RestoreBindPose( const std::shared_ptr<Assets::SkeletonAsset>& skeleton,
                          const ReferencePoseOnDisk& onDisk, const ResidentSkeletonClips& clips )
    {
        if ( !skeleton || skeleton->GetSkeleton() == nullptr ||
             skeleton->GetSkeleton()->GetBones().size() != onDisk.Binds.size() ||
             onDisk.Names.size() != onDisk.Binds.size() )
            return false;
        // Names first, through a name no bone holds: a discarded swap (A <-> B) would otherwise refuse its own
        // first step as a duplicate.
        const size_t                                 count = onDisk.Names.size();
        std::vector<Animation::Timeline::BoneRename> discarded; // memory name -> the file's, one simultaneous map
        for ( uint32_t i = 0; i < count; ++i )
            if ( const std::string& name = skeleton->GetSkeleton()->GetBones()[i].Name; name != onDisk.Names[i] )
                discarded.push_back( { name, onDisk.Names[i] } );
        for ( uint32_t i = 0; i < count; ++i )
            if ( skeleton->GetSkeleton()->GetBones()[i].Name != onDisk.Names[i] )
                (void)skeleton->RenameBone( i, std::format( "\x01restoring {}", i ) );
        for ( uint32_t i = 0; i < count; ++i )
            if ( skeleton->GetSkeleton()->GetBones()[i].Name != onDisk.Names[i] )
                (void)skeleton->RenameBone( i, onDisk.Names[i] );
        // The loaded clips were renamed with the bones (the Rename's transaction), so they are renamed back.
        if ( clips.Loaded != nullptr )
            (void)Assets::RenameBonesInResidentClips( clips.Skeleton, discarded, *clips.Loaded );
        for ( uint32_t i = 0; i < count; ++i )
            if ( skeleton->GetSkeleton()->GetBones()[i].LocalBindTransform != onDisk.Binds[i] )
                (void)skeleton->SetLocalBindTransform( i, onDisk.Binds[i] );
        CommandHistory::Get().DropFor( skeleton.get() );
        return true;
    }
} // namespace Desert::Editor
