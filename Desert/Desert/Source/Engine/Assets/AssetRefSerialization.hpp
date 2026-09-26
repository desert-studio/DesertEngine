#pragma once

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Common/Json/Document.hpp>
#include <Engine/Assets/AssetGuidRef.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace Desert::Assets
{
    /**
     * @brief Where a stored {Guid, Path} reference sits, so a refusal names it instead of a bare GUID:
     * what it references ("shader", "scene"), the field that holds it ("Material.Shader") and whose field
     * it is ("entity 'Cube'", "material 'Rock.dmat'").
     */
    struct AssetRefSite
    {
        std::string_view Kind;
        std::string_view Field;
        std::string_view Context;
    };

    /**
     * @brief GUID -> the runtime binding of the referenced asset (a shader's name, a scene's path), or nullopt
     * when nothing the caller knows carries that GUID. INJECTED rather than a registry call so the rule below
     * is linkable on its own: the scene serializer passes the content registry, the shader one the asset
     * manager, and a test passes a map.
     */
    using AssetGuidResolver = std::function<std::optional<std::string>( const Common::Content::AssetGuid& )>;

    // THE ONE RULE for writing and reading a {Guid, Path} reference (SCNE 31 and every text kind that names
    // another). Written: the referenced file's header GUID, and its ROOT-TAGGED stable key for the reader,
    // never the absolute runtime path, which names a directory that exists on one machine. Read: by GUID
    // alone; the path is a hint that appears only in the refusal, so a moved or renamed file still resolves
    // and a file that later took the old name is never bound. Both refuse by the site's name.
    [[nodiscard]] Common::ResultStr<AssetGuidRef> WriteAssetGuidRef( const Common::Content::AssetGuid& guid,
                                                                     const std::filesystem::path&      runtimePath,
                                                                     const AssetRefSite&               site );
    [[nodiscard]] Common::ResultStr<std::string>
    ResolveAssetGuidRef( const AssetGuidRef& ref, const AssetGuidResolver& resolver, const AssetRefSite& site );

    // A stored {Guid, Path} read STRICTLY (both members, both strings, nothing else): any other shape is an Issue
    // at the reference's own path and no reference - never a non-string GUID read as "" and refused as "states
    // no GUID" without saying which value the file holds.
    [[nodiscard]] std::optional<AssetGuidRef> ReadAssetGuidRef( const Common::Json::Node& stored,
                                                                Common::Json::Issues&     issues );
} // namespace Desert::Assets
