#pragma once

#include <string>

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
} // namespace Desert::Assets
