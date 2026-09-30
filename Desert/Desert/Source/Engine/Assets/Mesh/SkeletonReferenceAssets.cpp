#include "SkeletonReferenceAssets.hpp"

#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/Mesh/AnimationAsset.hpp>
#include <Engine/Assets/Mesh/SkeletonAsset.hpp>
#include <Engine/Assets/Mesh/SkinnedMeshAsset.hpp>
#include <Engine/Assets/Serialization/MeshBinary.hpp>
#include <Engine/Assets/Serialization/Skeleton.hpp>

#include <Common/Content/CanonicalText.hpp>
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
        // THE FILE IS THE BASE, NOT THE ASSET: bones, GUID and Import are rewritten exactly as the file states
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
        // THE COOKED FILE IS THE BASE: everything but the header's SkeletonGuid is rewritten as it reads.
        const std::filesystem::path file = ContentRegistry::FileToOpen( skmeshPath );
        if ( skeleton.IsNull() )
            return Common::MakeFormattedError<bool>(
                 "mesh '{}' was not saved: a skinned mesh must name a skeleton (the GUID is null)", file.string() );
        auto raw = Common::Utils::FileSystem::ReadFileContent( file );
        if ( !raw )
            return Common::MakeFormattedError<bool>( "mesh '{}' was not saved: {}", file.string(), raw.GetError() );
        auto decoded = DecodeMeshBinary( raw.GetValue(), file.string() );
        if ( !decoded )
            return Common::MakeFormattedError<bool>( "mesh '{}' was not saved: {}", file.string(),
                                                     decoded.GetError() );
        MeshAssetData data = decoded.ExtractValue();
        if ( !data.IsSkinned )
            return Common::MakeFormattedError<bool>(
                 "mesh '{}' was not saved: it is a static mesh, and only a skinned mesh names a skeleton",
                 file.string() );
        data.Skeleton = skeleton;
        if ( const auto written = Common::Utils::FileSystem::WriteContentToFileAtomic( file, EncodeMeshBinary( data ) );
             !written )
            return Common::MakeFormattedError<bool>( "mesh '{}' was not saved: {}", file.string(),
                                                     written.GetError() );
        return BOOLSUCCESS;
    }
} // namespace Desert::Assets::Serialization
