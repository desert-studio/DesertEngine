#pragma once

#include "CookVerdict.hpp"
#include "IAssetImporter.hpp"

#include <filesystem>

namespace Desert::Editor
{
    /**
     * @brief THE IES IMPORT (UE UTextureLightProfileFactory): an IESNA LM-63 `.ies` becomes the `.dies` beside it
     * (Assets::IesProfileAsset, same stem), the asset a light's IES Profile slot names. The `.ies` stays as the
     * asset's source, as a texture's image does.
     *
     * A RE-IMPORT KEEPS THE GUID: the `.dies` already beside the source keeps its header GUID, so every light
     * that names it still does. ImportPass::Cook writes nothing (ImportManager::Import's contract): a source with
     * no current `.dies` is Failed with a log naming the re-import. Up to date = the `.dies` is not older than
     * the `.ies`, unless @p force.
     */
    [[nodiscard]] std::filesystem::path IesProfileAssetPathFor( const std::filesystem::path& ies );

    [[nodiscard]] CookVerdict ImportIesProfile( const std::filesystem::path& ies, ImportPass pass, bool force );
} // namespace Desert::Editor
