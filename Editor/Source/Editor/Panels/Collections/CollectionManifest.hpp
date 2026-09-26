#pragma once

#include <Common/Json/Json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The `collection.json` manifest: its ONE shape, its ONE writer and its ONE reader.
//
// FbxMeshSplitter writes it and the Collections panel reads it. The splitter used to concatenate the JSON by
// hand with no escaping, so a collection folder, texture stem or path holding a quote or a backslash produced
// a file the strict reader refused — and the panel dropped the whole collection with a log line. Both ends now
// go through this header, so what one writes the other accepts by construction, and a test pins that.
//
// Header-only on purpose: the splitter is an engine-free tool that compiles the JSON facade in as one source
// and links nothing from Editor.
namespace Desert::Editor
{
    struct CollectionManifestItem
    {
        std::string                Name;
        std::optional<std::string> Category;
        std::string                Mesh; // working-dir-relative source path, forward slashes
        std::optional<std::string> Thumbnail;
        std::optional<int>         Material; // index into CollectionManifest::Materials (the mesh's PBR material)
    };

    // A PBR material the splitter detected from the pack's texture files (paths by filename suffix). The editor
    // materializes these into real .demat assets (the engine owns that format; the tool stays engine-free).
    // Cutout/foliage carries AlphaCutoff (TwoSided is reserved for a future shader feature).
    struct CollectionManifestMaterial
    {
        std::string                Name;
        std::optional<std::string> Albedo, Opacity, Normal, Roughness, Metallic, AO;
        std::optional<float>       AlphaCutoff;
        std::optional<bool>        TwoSided;
    };

    struct CollectionManifest
    {
        std::string                                            Name;
        std::optional<std::string>                             Author;
        std::optional<std::vector<CollectionManifestMaterial>> Materials;
        std::vector<CollectionManifestItem>                    Items;
    };

    [[nodiscard]] inline std::string WriteCollectionManifest( const CollectionManifest& manifest )
    {
        return Common::Json::Write( manifest );
    }

    // Strict: an unknown key or a member of the wrong type refuses, and the error names the member.
    [[nodiscard]] inline Common::ResultStr<CollectionManifest> ReadCollectionManifest( std::string_view json )
    {
        return Common::Json::Read<CollectionManifest>( json );
    }
} // namespace Desert::Editor
