#include "SkeletonReference.hpp"

#include <algorithm>
#include <format>

namespace Desert::Animation
{
    namespace
    {
        std::string_view NameOr( const SkeletonAssetRef& ref, std::string_view fallback )
        {
            return ref.Name.empty() ? fallback : std::string_view( ref.Name );
        }
    } // namespace

    Common::BoolResultStr ClipPlaysOnMesh( const SkeletonAssetRef&                     clipSkeleton,
                                           const SkeletonAssetRef&                     meshSkeleton,
                                           std::span<const Common::Content::AssetGuid> meshSkeletonCompatible )
    {
        const std::string_view clipName = NameOr( clipSkeleton, "<unnamed clip skeleton>" );
        const std::string_view meshName = NameOr( meshSkeleton, "<unnamed mesh skeleton>" );

        if ( clipSkeleton.Guid.IsNull() )
        {
            return Common::MakeFormattedError<bool>(
                 "the clip names no skeleton ('{}'), so it plays on no mesh; the mesh's skeleton is '{}'.",
                 clipName, meshName );
        }
        if ( meshSkeleton.Guid.IsNull() )
        {
            return Common::MakeFormattedError<bool>(
                 "the mesh names no skeleton ('{}'), so no clip plays on it; the clip's skeleton is '{}'.",
                 meshName, clipName );
        }
        if ( clipSkeleton.Guid == meshSkeleton.Guid )
            return Common::MakeSuccess( true );

        if ( std::ranges::find( meshSkeletonCompatible, clipSkeleton.Guid ) != meshSkeletonCompatible.end() )
            return Common::MakeSuccess( true );

        return Common::MakeFormattedError<bool>(
             "the clip's skeleton '{}' is not the mesh's skeleton '{}' and is not in its CompatibleSkeletons.",
             clipName, meshName );
    }

    Common::BoolResultStr CheckSkeletonAssignment( const Skeleton& skeleton, std::string_view skeletonName,
                                                   std::span<const RequiredBone> required,
                                                   std::string_view              assetName )
    {
        std::string missing;
        std::string misParented;
        const auto& bones = skeleton.GetBones();

        for ( const RequiredBone& need : required )
        {
            const std::optional<uint32_t> index = skeleton.FindBoneIndex( need.Name );
            if ( !index )
            {
                missing += missing.empty() ? "" : ", ";
                missing += need.Name;
                continue;
            }
            if ( !need.Parent )
                continue;

            const uint32_t    parent     = skeleton.ResolveParent( *index );
            const std::string actualName = parent == Skeleton::NO_PARENT ? std::string() : bones[parent].Name;
            if ( actualName != *need.Parent )
            {
                misParented += misParented.empty() ? "" : ", ";
                misParented += std::format( "{} (needs parent '{}', skeleton has '{}')", need.Name,
                                            need.Parent->empty() ? "<root>" : *need.Parent,
                                            actualName.empty() ? "<root>" : actualName );
            }
        }

        if ( missing.empty() && misParented.empty() )
            return Common::MakeSuccess( true );

        return Common::MakeFormattedError<bool>(
             "'{}' cannot reference skeleton '{}': missing bones [{}]; mis-parented bones [{}].", assetName,
             skeletonName, missing.empty() ? "none" : missing, misParented.empty() ? "none" : misParented );
    }

    std::vector<size_t> FindSkeletonsBySignature( uint64_t                           signature,
                                                  std::span<const SkeletonCandidate> skeletons )
    {
        std::vector<size_t> found;
        if ( signature == 0 )
            return found;
        for ( size_t i = 0; i < skeletons.size(); ++i )
            if ( skeletons[i].Signature == signature )
                found.push_back( i );
        return found;
    }

    Common::ResultStr<Common::Content::AssetGuid>
    MigrateSkeletonReference( std::string_view referencingPath, uint64_t legacySignature,
                              std::span<const SkeletonCandidate> skeletons )
    {
        using Common::Content::AssetGuid;
        if ( legacySignature == 0 )
        {
            return Common::MakeFormattedError<AssetGuid>(
                 "{}: its legacy skeleton signature is 0 — it names no skeleton; nothing to migrate to.",
                 referencingPath );
        }

        const std::vector<size_t> matches = FindSkeletonsBySignature( legacySignature, skeletons );
        if ( matches.size() == 1 )
            return Common::MakeSuccess( AssetGuid( skeletons[matches.front()].Guid ) );

        if ( matches.empty() )
        {
            return Common::MakeFormattedError<AssetGuid>(
                 "{}: no .skeleton among {} registered has signature {}; import or restore that skeleton first.",
                 referencingPath, skeletons.size(), legacySignature );
        }

        std::string paths;
        for ( const size_t i : matches )
        {
            paths += paths.empty() ? "" : ", ";
            paths += skeletons[i].Path;
        }
        return Common::MakeFormattedError<AssetGuid>(
             "{}: signature {} is AMBIGUOUS — {} skeletons carry it: [{}]; the migrator does not guess, keep one.",
             referencingPath, legacySignature, matches.size(), paths );
    }
} // namespace Desert::Animation
