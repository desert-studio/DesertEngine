#include "SkeletonReferenceAssets.hpp"

#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/Mesh/AnimationAsset.hpp>
#include <Engine/Assets/Mesh/SkeletonAsset.hpp>
#include <Engine/Assets/Mesh/SkinnedMeshAsset.hpp>
#include <Engine/Assets/MeshSourceAsset.hpp>
#include <Engine/Assets/RetargetAsset.hpp>
#include <Engine/Assets/Serialization/AnimationClipBuild.hpp>
#include <Engine/Assets/Serialization/AnimationClipWrite.hpp>
#include <Engine/Assets/Serialization/ImportRecord.hpp>
#include <Engine/Assets/Serialization/MeshBinary.hpp>
#include <Engine/Assets/Serialization/Retarget.hpp>
#include <Engine/Assets/Serialization/Skeleton.hpp>

#include <Common/Content/CanonicalText.hpp>
#include <Common/Content/ImportRecord.hpp>
#include <Common/Content/MeshBinaryHeader.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/Core.hpp> // BOOLSUCCESS
#include <Common/Utilities/FileSystem.hpp>

#include <algorithm>
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
        // A bone binding names only its bone; the parent is the skeleton's business (RequiredBone::Parent =
        // nullopt).
        std::vector<Animation::RequiredBone> required;
        std::unordered_set<std::string>      seen;
        for ( const auto& binding : clip.GetClip().Sequence.Bindings )
            if ( binding.Kind == Animation::Timeline::BindingKind::Bone && !binding.Locator.empty() &&
                 seen.insert( binding.Locator ).second )
                required.push_back( Animation::RequiredBone{ binding.Locator, std::nullopt } );
        return required;
    }

    SkeletonReferrers ReferrersOfSkeleton( const Common::Content::AssetGuid& skeleton, AssetManager* loaded )
    {
        SkeletonReferrers referrers;
        referrers.Loaded = loaded;
        for ( const ContentRegistry::PickerRow& row :
              ContentRegistry::Rows( Common::Content::ContentKind::Animation ) )
            if ( row.Skeleton == skeleton )
                referrers.ClipFiles.push_back( row.Path );
        // A retarget states its source rig in its body (SourceSkeleton), not in a registry tag: every one is
        // listed and RenameBonesInSkeletonAssets keeps the ones whose file names this skeleton.
        for ( const ContentRegistry::PickerRow& row :
              ContentRegistry::Rows( Common::Content::ContentKind::Retarget ) )
            referrers.RetargetFiles.push_back( row.Path );
        return referrers;
    }

    namespace
    {
        /// The one rename rule every name holder shares: @p name takes the `To` of the rename whose `From` it is.
        bool RenameBoneName( std::string& name, const std::span<const Animation::Timeline::BoneRename> renames )
        {
            const auto rename =
                 std::find_if( renames.begin(), renames.end(), [&]( const auto& r ) { return r.From == name; } );
            if ( rename == renames.end() || rename->From == rename->To )
                return false;
            name = rename->To;
            return true;
        }

        Common::BoolResultStr RenameBonesInRetargetFiles( const Common::Content::AssetGuid& skeleton,
                                                          std::span<const Animation::Timeline::BoneRename> renames,
                                                          const SkeletonReferrers& referrers )
        {
            for ( const std::filesystem::path& retargetFile : referrers.RetargetFiles )
            {
                const std::filesystem::path file = ContentRegistry::FileToOpen( retargetFile );
                auto                        data = Serialization::LoadRetargetFile( file );
                if ( !data )
                    return Common::MakeFormattedError<bool>( "bone rename: retarget '{}' was not read: {}",
                                                             file.string(), data.GetError() );
                Serialization::RetargetAssetData renamed = data.ExtractValue();
                const auto source = Common::Content::AssetGuidFromText( renamed.SourceSkeleton.Guid );
                if ( !source || source.GetValue() != skeleton ||
                     Serialization::RenameSourceBonesInRetarget( renamed, renames ) == 0 )
                    continue;
                if ( const auto written = Serialization::SaveRetargetFile( file, renamed ); !written )
                    return Common::MakeFormattedError<bool>( "bone rename: retarget '{}' was not written: {}",
                                                             file.string(), written.GetError() );
                // A RESIDENT RETARGET RE-READS ITS FILE: the Load bumps its Revision, which is what the ECS system
                // rebuilds an entity's Retargeter on.
                if ( referrers.Loaded == nullptr )
                    continue;
                for ( const auto& [handle, asset] : referrers.Loaded->FindAllByType<RetargetAsset>() )
                {
                    if ( !asset || ContentRegistry::FileToOpen( asset->GetMetadata().Filepath ) != file )
                        continue;
                    if ( const auto reloaded = asset->Load(); !reloaded )
                        return Common::MakeFormattedError<bool>( "bone rename: retarget '{}' was written, its "
                                                                 "resident asset did not re-read it: {}",
                                                                 file.string(), reloaded.GetError() );
                    asset->ResolveDependencies( *referrers.Loaded );
                }
            }
            return BOOLSUCCESS;
        }
    } // namespace

    Common::BoolResultStr
    RenameBonesInSkeletonAssets( const Common::Content::AssetGuid&                      skeleton,
                                 const std::span<const Animation::Timeline::BoneRename> renames,
                                 const SkeletonReferrers&                               referrers )
    {
        if ( renames.empty() )
            return BOOLSUCCESS;
        for ( const std::filesystem::path& clipFile : referrers.ClipFiles )
        {
            // The FILE is renamed from the file, never from a resident clip: an open clip editor's unsaved edits
            // are its own Save's, and this writes the rename alone.
            const std::filesystem::path file = ContentRegistry::FileToOpen( clipFile );
            auto                        raw  = Common::Utils::FileSystem::ReadFileContent( file );
            if ( !raw )
                return Common::MakeFormattedError<bool>( "bone rename: clip '{}' was not read: {}", file.string(),
                                                         raw.GetError() );
            auto data = Serialization::ReadAnimationJson( raw.GetValue() );
            if ( !data )
                return Common::MakeFormattedError<bool>( "bone rename: clip '{}': {}", file.string(),
                                                         data.GetError() );
            auto clip = Serialization::BuildClipFromAssetData( data.GetValue() );
            if ( !clip )
                return Common::MakeFormattedError<bool>( "bone rename: clip '{}': {}", file.string(),
                                                         clip.GetError() );
            Animation::AnimationClip renamed = clip.ExtractValue();
            if ( renamed.Skeleton != skeleton ||
                 Animation::Timeline::RenameBoneLocators( renamed.Sequence, renames ) == 0 )
                continue;
            if ( const auto written = Serialization::SaveClipToFile( file, renamed ); !written )
                return Common::MakeFormattedError<bool>( "bone rename: clip '{}' was not written: {}",
                                                         file.string(), written.GetError() );
        }
        if ( referrers.Loaded != nullptr )
            for ( const auto& [handle, clip] : referrers.Loaded->FindAllByType<AnimationAsset>() )
                if ( clip && clip->GetSkeleton() == skeleton )
                    (void)clip->RenameBones( renames );
        return RenameBonesInRetargetFiles( skeleton, renames, referrers );
    }
} // namespace Desert::Assets

namespace Desert::Assets::Serialization
{
    std::size_t RenameSourceBonesInRetarget( RetargetAssetData&                                     data,
                                             const std::span<const Animation::Timeline::BoneRename> renames )
    {
        std::size_t moved = RenameBoneName( data.SourcePelvisBone, renames ) ? 1U : 0U;
        for ( RetargetBoneOffsetData& offset : data.SourceRetargetPose.BoneOffsets )
            moved += RenameBoneName( offset.Bone, renames ) ? 1U : 0U;
        for ( RetargetChainData& chain : data.Chains )
        {
            moved += RenameBoneName( chain.SourceStartBone, renames ) ? 1U : 0U;
            moved += RenameBoneName( chain.SourceEndBone, renames ) ? 1U : 0U;
        }
        for ( RetargetBoneRenameData& pair : data.BoneRenames )
            moved += RenameBoneName( pair.SourceBone, renames ) ? 1U : 0U;
        return moved;
    }

    Common::BoolResultStr SaveSkeletonAsset( const SkeletonAsset& skeleton, const SkeletonReferrers& referrers )
    {
        // THE FILE IS THE BASE, NOT THE ASSET: bone structure, GUID and signature are rewritten exactly as the
        // file states them, so a save from the Skeleton Editor can never drop the import record or re-mint the
        // identity.
        const std::filesystem::path file = ContentRegistry::FileToOpen( skeleton.GetMetadata().Filepath );
        auto                        raw  = Common::Utils::FileSystem::ReadFileContent( file );
        if ( !raw )
            return Common::MakeFormattedError<bool>( "skeleton '{}' was not saved: {}", file.string(),
                                                     raw.GetError() );
        auto read = ReadSkeletonJson( raw.GetValue() );
        if ( !read )
            return Common::MakeFormattedError<bool>( "skeleton '{}' was not saved: {}", file.string(),
                                                     read.GetError() );

        SkeletonAssetData                            data = read.ExtractValue();
        std::vector<Animation::Timeline::BoneRename> renames;
        // THE REFERENCE POSE AND THE BONE NAMES ARE AUTHORED (Skeleton Editor, UE's Skeleton Tree): every bone's
        // LocalBindTransform and Name come from the rig in memory, index for index. The parent links are the
        // file's, so a file of another bone count (another rig written over it) is refused rather than
        // half-merged; a name that differs at the same index is a Rename Bone, carried into the referrers. The
        // OffsetMatrix stays the file's: it is the mesh's inverse bind the skin was cooked against, and the edit
        // is what moves the skin away from it (GizmoController's rest-pose edit does the same in memory).
        if ( const Animation::Skeleton* rig = skeleton.GetSkeleton() )
        {
            const auto& bones = rig->GetBones();
            if ( bones.size() != data.Bones.size() )
                return Common::MakeFormattedError<bool>(
                     "skeleton '{}' was not saved: the file has {} bones, the rig in memory {}", file.string(),
                     data.Bones.size(), bones.size() );
            for ( size_t i = 0; i < bones.size(); ++i )
            {
                if ( bones[i].Name != data.Bones[i].Name )
                {
                    renames.push_back( { data.Bones[i].Name, bones[i].Name } );
                    data.Bones[i].Name = bones[i].Name;
                }
                data.Bones[i].LocalBindTransform = bones[i].LocalBindTransform;
            }
            if ( !renames.empty() )
                data.Signature = Animation::Skeleton::ComputeSignature( data.Bones );
        }
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
        if ( renames.empty() )
            return BOOLSUCCESS;
        const auto guid = Common::Content::AssetGuidFromText( data.Header ? data.Header->Guid : std::string() );
        if ( !guid )
            return Common::MakeFormattedError<bool>( "skeleton '{}' was saved, its renamed bones were not carried "
                                                     "into its assets: the header GUID: {}",
                                                     file.string(), guid.GetError() );
        if ( auto carried = RenameBonesInSkeletonAssets( guid.GetValue(), renames, referrers ); !carried )
            return Common::MakeFormattedError<bool>( "skeleton '{}' was saved; {}", file.string(),
                                                     carried.GetError() );
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
