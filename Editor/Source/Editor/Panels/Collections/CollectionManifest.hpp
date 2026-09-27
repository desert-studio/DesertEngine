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
    /**
     * @brief The manifest's generation.
     *
     *   1 - (unversioned) name, author, materials, items {name, category, mesh, thumbnail, material}.
     *   2 - the Version member itself, and an item may name the foliage type it paints with (FO-2).
     *
     * Any other value, or a file without the member, is refused: there is no reader for an older shape.
     */
    inline constexpr int kCollectionManifestVersion = 2;

    // An asset named the way every text asset of this project names another: GUID (identity) and path
    // (relative to the assets root, forward slashes; what a reader shows when the GUID does not resolve).
    struct CollectionManifestAssetRef
    {
        std::string Guid;
        std::string Path;
    };

    struct CollectionManifestItem
    {
        std::string                Name;
        std::optional<std::string> Category;
        std::string                Mesh; // working-dir-relative source path, forward slashes
        std::optional<std::string> Thumbnail;
        std::optional<int>         Material; // index into CollectionManifest::Materials (the mesh's PBR material)
        // The `.defoliage` this item paints with (UE: a collection of FoliageTypes). Recorded by the editor the
        // first time the item reaches the foliage palette, so a type tuned later is the one the next drop reuses.
        std::optional<CollectionManifestAssetRef> FoliageType;
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
        int                                                    Version = kCollectionManifestVersion;
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
    // A Version other than kCollectionManifestVersion (or none) refuses, naming both numbers.
    [[nodiscard]] inline Common::ResultStr<CollectionManifest> ReadCollectionManifest( std::string_view json )
    {
        auto read = Common::Json::Read<CollectionManifest>( json );
        if ( !read )
            return read;
        if ( read.GetValue().Version != kCollectionManifestVersion )
            return Common::MakeError<CollectionManifest>(
                 "collection.json Version " + std::to_string( read.GetValue().Version ) + " is not " +
                 std::to_string( kCollectionManifestVersion ) + "; re-run FbxMeshSplitter on the pack" );
        return read;
    }
} // namespace Desert::Editor
