#pragma once

#include <Editor/Panels/Collections/CollectionManifest.hpp>
#include <Engine/Assets/AssetGuidRef.hpp>
#include <Engine/Assets/Serialization/FoliageType.hpp>
#include <Common/Core/ResultStr.hpp>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <vector>

// A collection dropped on the foliage palette becomes foliage types (UE: a folder of FoliageType assets dragged
// onto the palette). Pure file logic: the mesh lookup, which needs the asset manager and may cook, is the
// caller's, so a suite can drive this without an editor.
namespace Desert::Editor
{
    // The mesh an item names, as the `.defoliage` will name it. The editor resolves (and cooks) the item's
    // source; an error refuses the whole drop, naming the item.
    using CollectionMeshResolver =
         std::function<Common::ResultStr<Assets::AssetGuidRef>( const CollectionManifestItem& )>;

    struct CollectionFoliageTypes
    {
        std::vector<Assets::Serialization::FoliageTypeFile> Types; // one per item, in item order
        bool ManifestChanged = false; // an item gained its FoliageType record; the caller saves the manifest
    };

    /**
     * @brief Every item's foliage type.
     *
     * An item that records one gets that file, after checking the file still states the recorded GUID (a
     * missing or re-minted file refuses, naming the item: silently making a fresh type would drop the tuning
     * the record exists to keep). An item without a record gets the type FindOrCreateFoliageTypeFile finds or
     * makes for its mesh with default numbers, and the record is written into @p manifest.
     *
     * @param typesDir   where types are looked for and created (Constants::Path::FOLIAGE_TYPE_PATH)
     * @param assetsRoot what the records' paths are relative to (Constants::Path::ASSETS_PATH)
     */
    Common::ResultStr<CollectionFoliageTypes>
    ResolveCollectionFoliageTypes( CollectionManifest& manifest, const std::filesystem::path& typesDir,
                                   const std::filesystem::path& assetsRoot, const CollectionMeshResolver& meshOf );

    // Writes the manifest through its one writer, atomically.
    Common::BoolResultStr SaveCollectionManifest( const std::filesystem::path& file,
                                                  const CollectionManifest&    manifest );
} // namespace Desert::Editor
