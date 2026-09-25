#include "SkeletonAsset.hpp"
#include <Engine/Assets/Serialization/Skeleton.hpp>
#include <Engine/Assets/TextAssetHeaderIdentity.hpp>

#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/VFS.hpp>

namespace Desert::Assets
{
    SkeletonAsset::SkeletonAsset( const AssetPriority priority, const Common::Filepath& filepath )
         : AssetBase( priority, filepath, GetTypeID() )
    {
        // THE RIG'S IDENTITY IS ITS HEADER GUID (SKEL 1, T7e), adopted HERE for ControlRigAsset's reason: the
        // asset manager keys its handle lookup at creation. A file with no readable header keeps the
        // path-derived handle - the load refuses it by name, so none is ever READY under it.
        const Common::Content::AssetGuid guid = ReadTextHeaderGuid( m_Metadata.Filepath );
        if ( !guid.IsNull() )
            AdoptHandleFromFile( Common::UUID( static_cast<uint64_t>( Common::Content::HandleForGuid( guid ) ) ),
                                 Common::AssetHandle::StableKeyForPath( m_Metadata.Filepath ) );
    }

    Common::BoolResultStr SkeletonAsset::LoadFromFile()
    {
        // Through the VFS first, so a packaged build reads the rig out of its .dpak like every other asset,
        // then off the disk for a loose file the pak does not carry (T7e: it read only the disk).
        std::string text;
        if ( const auto packed = Common::Utils::VFS::Exists( m_Metadata.Filepath )
                                      ? Common::Utils::VFS::ReadFile( m_Metadata.Filepath )
                                      : std::nullopt;
             packed.has_value() )
            text = packed.value();
        else
        {
            auto raw = Common::Utils::FileSystem::ReadFileContent( m_Metadata.Filepath );
            if ( !raw )
                return Common::MakeError( raw.GetError() );
            text = raw.ExtractValue();
        }

        auto read = Serialization::ReadSkeletonJson( text );
        if ( !read )
            return Common::MakeFormattedError<bool>( "'{}': {}", m_Metadata.Filepath.string(), read.GetError() );

        auto data = read.ExtractValue();

        m_Skeleton = std::make_unique<Animation::Skeleton>( std::move( data.Bones ) );
        // Taken from the bones that were just read, never from `data.Signature`: the file's own field is
        // what a cook WROTE, and this is what the rig in memory IS. A mesh is matched against the second.
        m_Signature = m_Skeleton->GetSignature();

        return BOOLSUCCESS;
    }

    Common::BoolResultStr SkeletonAsset::Unload()
    {
        // WAS `return BOOLSUCCESS` — WITHOUT A SEMICOLON. It compiled only because `#define BOOLSUCCESS
        // Common::MakeSuccess( true );` carries one inside the macro, and it is the clearest evidence in
        // the set that these thirteen bodies were written with no caller and never read again: every other
        // one of them writes the semicolon.
        //
        // It also leaked the one thing this class owns. `m_Skeleton` is a `unique_ptr<Animation::Skeleton>`
        // holding the whole bone hierarchy, and nothing released it.
        m_Skeleton.reset();
        // `m_Signature` is deliberately NOT cleared — it is this rig's identity rather than its payload,
        // and clearing it is the defect GetSignature's comment records. The suite
        // `AssetEviction.AnUnloadedAssetStopsAnsweringWithItsPayload` states this carve-out next to the
        // fields that DO go, and `AssetEviction.ASkinnedMeshRebindsItsRigAfterASweepHasReleasedBoth`
        // asserts it over a rig that was really loaded — so the next reader has to decide, not infer.
        return BOOLSUCCESS;
    }
} // namespace Desert::Assets
