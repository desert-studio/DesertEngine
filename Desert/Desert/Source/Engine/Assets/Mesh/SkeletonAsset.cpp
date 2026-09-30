#include "SkeletonAsset.hpp"
#include <Engine/Assets/Serialization/Skeleton.hpp>
#include <Engine/Assets/TextAssetHeaderIdentity.hpp>

#include <Common/Content/TextAssetHeader.hpp>

#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/VFS.hpp>

namespace Desert::Assets
{
    SkeletonAsset::SkeletonAsset( const Common::Filepath& filepath ) : AssetBase( filepath, GetTypeID() )
    {
        // THE RIG'S IDENTITY IS ITS HEADER GUID (SKEL 1, T7e), adopted HERE for ControlRigAsset's reason: the
        // asset manager keys its handle lookup at creation. A file with no readable header keeps the
        // path-derived handle - the load refuses it by name, so none is ever READY under it.
        if ( const TextAssetIdentity identity = ReadTextAssetIdentity( m_Metadata.Filepath );
             !identity.Guid.IsNull() )
            AdoptHandleFromFile( identity.Handle(), identity.StableKey() );
    }

    Common::BoolResultStr SkeletonAsset::LoadFromFile()
    {
        // The old path of a moved asset reads the file where it now lives, through the registry - the same
        // file the constructor took the identity from (ReadTextAssetIdentity).
        const std::filesystem::path file = ContentRegistry::FileToOpen( m_Metadata.Filepath );
        // Through the VFS first, so a packaged build reads the rig out of its .dpak like every other asset,
        // then off the disk for a loose file the pak does not carry (T7e: it read only the disk).
        std::string text;
        if ( const auto packed =
                  Common::Utils::VFS::Exists( file ) ? Common::Utils::VFS::ReadFile( file ) : std::nullopt;
             packed.has_value() )
            text = packed.value();
        else
        {
            auto raw = Common::Utils::FileSystem::ReadFileContent( file );
            if ( !raw )
                return Common::MakeError( raw.GetError() );
            text = raw.ExtractValue();
        }

        auto read = Serialization::ReadSkeletonJson( text );
        if ( !read )
            return Common::MakeFormattedError<bool>( "'{}': {}", file.string(), read.GetError() );

        auto data = read.ExtractValue();

        // A RIG THAT IS LOADED IS RE-READ AT THE SAME ADDRESS (UE: Reimport rewrites the USkeleton in its own
        // UObject). Its readers hold the object itself — SkinnedMesh's `const Skeleton*`, Animator's
        // `const Skeleton&` — so replacing it would leave every one of them on freed memory. What tells them
        // the bones moved is the signature below: AnimationECSSystem rebuilds an Animator whose
        // `AnimationComponent::BuiltSkeletonSignature` no longer matches. `Load()` on a loaded rig IS the
        // reload (ImportOptionsDialog's ReloadLoaded), exactly as it is for AnimationAsset's clip; `Unload`
        // first would free the object. Written only after the file parsed, so a failed reload keeps the rig.
        if ( m_Skeleton )
            *m_Skeleton = Animation::Skeleton( std::move( data.Bones ) );
        else
            m_Skeleton = std::make_unique<Animation::Skeleton>( std::move( data.Bones ) );
        // Taken from the bones that were just read, never from `data.Signature`: the file's own field is
        // what a cook WROTE, and this is what the rig in memory IS. A mesh is matched against the second.
        m_Signature = m_Skeleton->GetSignature();

        // The references (SKEL 2) resolve by GUID; the stored path is only for the reader. A GUID that does not
        // parse is a broken file, refused by name rather than read as "no reference".
        Common::Content::AssetGuid preview;
        if ( data.PreviewMesh )
        {
            auto guid = Common::Content::AssetGuidFromText( data.PreviewMesh->Guid );
            if ( !guid )
                return Common::MakeFormattedError<bool>( "'{}': PreviewMesh '{}': {}", file.string(),
                                                         data.PreviewMesh->Path, guid.GetError() );
            preview = guid.GetValue();
        }
        std::vector<Common::Content::AssetGuid> compatible;
        compatible.reserve( data.CompatibleSkeletons.size() );
        for ( const AssetGuidRef& ref : data.CompatibleSkeletons )
        {
            auto guid = Common::Content::AssetGuidFromText( ref.Guid );
            if ( !guid )
                return Common::MakeFormattedError<bool>( "'{}': CompatibleSkeletons '{}': {}", file.string(),
                                                         ref.Path, guid.GetError() );
            compatible.push_back( guid.GetValue() );
        }
        m_PreviewMesh         = preview;
        m_CompatibleSkeletons = std::move( compatible );

        return BOOLSUCCESS;
    }

    Common::Content::AssetGuid SkeletonAsset::GetPreviewMesh() const
    {
        return m_PreviewMesh;
    }

    std::span<const Common::Content::AssetGuid> SkeletonAsset::GetCompatibleSkeletons() const
    {
        return m_CompatibleSkeletons;
    }

    void SkeletonAsset::SetPreviewMesh( Common::Content::AssetGuid mesh )
    {
        m_PreviewMesh = mesh;
    }

    void SkeletonAsset::SetCompatibleSkeletons( std::vector<Common::Content::AssetGuid> skeletons )
    {
        m_CompatibleSkeletons = std::move( skeletons );
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
