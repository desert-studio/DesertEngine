#pragma once

#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/ResultStr.hpp>

#include <format>
#include <string>
#include <string_view>

namespace Desert::Assets
{
    /**
     * @brief One text asset's reference to another, as the referencing file states it: the referenced
     * file's header GUID, which IS its identity (the asset adopts HandleForGuid of it in its constructor,
     * TextAssetHeaderIdentity.hpp), and the path it had when the reference was written.
     *
     * THE GUID RESOLVES; THE PATH IS FOR THE READER. A reference that resolved by path would break on a
     * rename and would bind whatever file later took the old name; one that carried only the GUID would
     * leave a hand-editor and every warning with thirty-two hex digits. So both are stored, and only one of
     * them is ever looked up. The path's root is the referencing format's to state (a `.retarget` states its
     * rig relative to the cooked meshes root), because that root differs by referenced kind.
     *
     * One type for every text kind that names another by GUID, so each format does not grow its own pair
     * with its own spelling of the two fields.
     */
    struct AssetGuidRef
    {
        std::string Guid;
        std::string Path;

        [[nodiscard]] bool operator==( const AssetGuidRef& ) const = default;
    };

    /**
     * @brief THE TARGET SKELETON A FORMAT STATES (UE: UAnimBlueprint::TargetSkeleton, UControlRig's preview
     * skeleton, the target IK Rig of a retargeter): the `.skeleton` whose bones the file names. Required -
     * a file that names bones but not their skeleton cannot be found by a Rename Bone (SkeletonReferrers looks
     * the skeleton up by this GUID). Refused: a GUID that is not well-formed or is null, an empty path, a
     * rooted path (leading separator or drive letter, on every platform) or one escaping the assets root.
     * @p what names the file in the message.
     */
    [[nodiscard]] inline Common::BoolResultStr CheckTargetSkeletonRef( const AssetGuidRef& ref,
                                                                       std::string_view    what )
    {
        if ( const auto guid = Common::Content::AssetGuidFromText( ref.Guid ); !guid || guid.GetValue().IsNull() )
            return Common::MakeError<bool>(
                 std::format( "{} states no well-formed TargetSkeleton GUID ('{}'); run "
                              "Tools/SceneMigrator",
                              what, ref.Guid ) );
        const std::string& p        = ref.Path;
        const bool         rooted   = !p.empty() && ( p.front() == '/' || p.front() == '\\' );
        const bool         lettered = p.size() >= 2 && p[1] == ':';
        if ( p.empty() || rooted || lettered || p.starts_with( ".." ) )
            return Common::MakeError<bool>(
                 std::format( "{}: TargetSkeleton path '{}' must be relative to the assets "
                              "root and must not escape it",
                              what, p ) );
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Assets
