#include "SkeletonReferenceAssets.hpp"

#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/Mesh/AnimationAsset.hpp>
#include <Engine/Assets/Mesh/SkeletonAsset.hpp>
#include <Engine/Assets/Mesh/SkinnedMeshAsset.hpp>
#include <Engine/Assets/MeshSourceAsset.hpp>
#include <Engine/Assets/Serialization/ImportRecord.hpp>
#include <Engine/Assets/Serialization/MeshBinary.hpp>
#include <Engine/Assets/Serialization/Skeleton.hpp>

#include <Common/Content/CanonicalText.hpp>
#include <Common/Content/ImportRecord.hpp>
#include <Common/Content/MeshBinaryHeader.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/Core.hpp> // BOOLSUCCESS
#include <Common/Utilities/FileSystem.hpp>

#include <unordered_set>

namespace Desert::Assets
{
    std::vector<Animation::RequiredBone> RequiredBonesOf( const SkinnedMeshAsset& mesh )
    {
        // THE MESH'S REFERENCE SKELETON IS ITS BOUND RIG: the .skmesh stores bone INDICES, the names and the
        // hierarchy they index live on the skeleton it was cooked against (UE USkeletalMesh::RefSkeleton). An
        // unbound mesh therefore requires nothing it can state - the Details slot asks after binding.
        std::vector<Animation::RequiredBone> required;
        const auto                           rig = mesh.GetSkeletonDependency().Cached.lock();
        if ( !rig || rig->GetSkeleton() == nullptr )
            return required;

        const Animation::Skeleton& skeleton = *rig->GetSkeleton();
        const auto&                bones    = skeleton.GetBones();
        required.reserve( bones.size() );
        for ( uint32_t i = 0; i < bones.size(); ++i )
        {
            const uint32_t parent = skeleton.ResolveParent( i );
            required.push_back( Animation::RequiredBone{
                 bones[i].Name, parent == Animation::Skeleton::NO_PARENT ? std::string() : bones[parent].Name } );
        }
        return required;
    }

    std::vector<Animation::RequiredBone> RequiredBonesOf( const AnimationAsset& clip )
    {
        // A track names only its bone; the parent is the skeleton's business (RequiredBone::Parent = nullopt).
        std::vector<Animation::RequiredBone> required;
        std::unordered_set<std::string>      seen;
        for ( const auto& track : clip.GetClip().Tracks )
            if ( !track.BoneName.empty() && seen.insert( track.BoneName ).second )
                required.push_back( Animation::RequiredBone{ track.BoneName, std::nullopt } );
        return required;
    }
} // namespace Desert::Assets

namespace Desert::Assets::Serialization
{

    Common::BoolResultStr SaveSkeletonAsset( const SkeletonAsset& skeleton )
    {
        // THE FILE IS THE BASE, NOT THE ASSET: bones, GUID and signature are rewritten exactly as the file states
        // them, so a save from the Skeleton Editor can never drop the import record or re-mint the identity.
        const std::filesystem::path file = ContentRegistry::FileToOpen( skeleton.GetMetadata().Filepath );
        auto                        raw  = Common::Utils::FileSystem::ReadFileContent( file );
        if ( !raw )
            return Common::MakeFormattedError<bool>( "skeleton '{}' was not saved: {}", file.string(),
                                                     raw.GetError() );
        auto read = ReadSkeletonJson( raw.GetValue() );
        if ( !read )
            return Common::MakeFormattedError<bool>( "skeleton '{}' was not saved: {}", file.string(),
                                                     read.GetError() );

        SkeletonAssetData data = read.ExtractValue();
        data.PreviewMesh.reset();
        if ( const auto preview = skeleton.GetPreviewMesh(); !preview.IsNull() )
            data.PreviewMesh = ContentRegistry::ReferenceTo( preview );
        data.CompatibleSkeletons.clear();
        for ( const auto& compatible : skeleton.GetCompatibleSkeletons() )
        {
            if ( compatible.IsNull() )
                return Common::MakeFormattedError<bool>(
                     "skeleton '{}' was not saved: CompatibleSkeletons holds a null reference", file.string() );
            data.CompatibleSkeletons.push_back( ContentRegistry::ReferenceTo( compatible ) );
        }

        const auto canonical = Common::Content::CanonicalJsonTextOfWriterOutput( WriteSkeletonJson( data ) );
        if ( !canonical )
            return Common::MakeError<bool>( canonical.GetError() );
        if ( const auto written =
                  Common::Utils::FileSystem::WriteContentToFileAtomic( file, canonical.GetValue() );
             !written )
            return Common::MakeFormattedError<bool>( "skeleton '{}' was not saved: {}", file.string(),
                                                     written.GetError() );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr SaveMeshSkeletonReference( const std::filesystem::path&     skmeshPath,
                                                     const Common::Content::AssetGuid skeleton )
    {
        const std::filesystem::path file = ContentRegistry::FileToOpen( skmeshPath );
        if ( skeleton.IsNull() )
            return Common::MakeFormattedError<bool>(
                 "mesh '{}' was not saved: a skinned mesh must name a skeleton (the GUID is null)",
                 file.string() );
        auto raw = Common::Utils::FileSystem::ReadFileContent( file );
        if ( !raw )
            return Common::MakeFormattedError<bool>( "mesh '{}' was not saved: {}", file.string(),
                                                     raw.GetError() );

        // A MESH SOURCE ASSET (an MSAS envelope, not a cooked DESTMESH): the skin's Skeleton IS the reference;
        // the .skmesh the deriver builds from it states the same GUID.
        if ( !std::string_view( raw.GetValue() )
                   .starts_with( std::string_view( Common::Content::kMeshBinaryMagic,
                                                   sizeof( Common::Content::kMeshBinaryMagic ) ) ) )
        {
            auto source = ReadMeshSourceAssetFile( file );
            if ( !source )
                return Common::MakeFormattedError<bool>( "mesh '{}' was not saved: {}", file.string(),
                                                         source.GetError() );
            MeshSourceAsset asset = source.ExtractValue();
            if ( !asset.Source.Skin )
                return Common::MakeFormattedError<bool>(
                     "mesh '{}' was not saved: it is a static mesh, and only a skinned mesh names a skeleton",
                     file.string() );
            asset.Source.Skin->Skeleton = skeleton;
            if ( const auto written = WriteMeshSourceAssetFile( file, asset ); !written )
                return Common::MakeFormattedError<bool>( "mesh '{}' was not saved: {}", file.string(),
                                                         written.GetError() );
            return BOOLSUCCESS;
        }

        // AN IMPORTED MESH (cooked DESTMESH beside its raw source): everything but the header's SkeletonGuid is
        // rewritten as it reads, AND the source's import record chooses the same skeleton (UE: the skeleton is
        // part of the asset's import settings, and Reimport repeats them) - else a re-import would bind the rig
        // the file matches and silently revert the artist's assignment.
        auto decoded = DecodeMeshBinary( raw.GetValue(), file.string() );
        if ( !decoded )
            return Common::MakeFormattedError<bool>( "mesh '{}' was not saved: {}", file.string(),
                                                     decoded.GetError() );
        MeshAssetData data = decoded.ExtractValue();
        if ( !data.IsSkinned )
            return Common::MakeFormattedError<bool>(
                 "mesh '{}' was not saved: it is a static mesh, and only a skinned mesh names a skeleton",
                 file.string() );
        const std::optional<std::filesystem::path> rawSource = Common::Content::MeshSourceBeside( file );
        if ( rawSource )
        {
            const auto record = ReadImportRecord( *rawSource );
            if ( !record )
                return Common::MakeFormattedError<bool>( "mesh '{}' was not saved: {}", file.string(),
                                                         record.GetError() );
            if ( record.GetValue() )
                if ( const auto chosen = SetImportRecordSkeleton( *rawSource, skeleton ); !chosen )
                    return Common::MakeFormattedError<bool>( "mesh '{}' was not saved: {}", file.string(),
                                                             chosen.GetError() );
        }
        data.Skeleton = skeleton;
        if ( const auto written =
                  Common::Utils::FileSystem::WriteContentToFileAtomic( file, EncodeMeshBinary( data ) );
             !written )
            return Common::MakeFormattedError<bool>( "mesh '{}' was not saved: {}", file.string(),
                                                     written.GetError() );
        return BOOLSUCCESS;
    }
} // namespace Desert::Assets::Serialization
