#pragma once

#include "CookVerdict.hpp"
#include "IAssetImporter.hpp"

#include <filesystem>

namespace Desert::Editor
{
    /**
     * @brief THE SPARSE VOLUME IMPORT (UE USparseVolumeTextureFactory over an OpenVDB file): a simulated grid in
     * the Mitsuba / EmberGen / Houdini `.vol` format becomes the `.dsvol` beside it (Assets::SparseVolumeAsset,
     * same stem), the asset a heterogeneous volume's Sparse Volume slot names. The bricking goes through the DDC
     * (Assets::ImportSparseVolumeCached): the same source bytes never brick twice.
     *
     * A RE-IMPORT KEEPS THE GUID: the `.dsvol` already beside the source keeps its envelope GUID, so every volume
     * that names it still does. ImportPass::Cook writes nothing (ImportManager::Import's contract): a source with
     * no current `.dsvol` is Failed with a log naming the re-import. Up to date = the `.dsvol` is not older than
     * the `.vol`, unless @p force.
     */
    [[nodiscard]] std::filesystem::path SparseVolumeAssetPathFor( const std::filesystem::path& vol );

    [[nodiscard]] CookVerdict ImportSparseVolume( const std::filesystem::path& vol, ImportPass pass, bool force );
} // namespace Desert::Editor
