#pragma once

#include <Engine/Assets/ItemProgress.hpp>
#include <array>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

#include <Common/Core/UUID.hpp>

namespace Desert::Editor
{
    // What one cook of one source did. `Fresh` and `Cooked` both leave a correct `.tex` on the disk; the
    // difference is whether this call paid for it, which is what an incremental cook has to be able to
    // report. `Unwritten` is kept apart from `Failed` for the reason `CookStats::StoreFailures` gives: a
    // source that cannot be decoded is broken content, a container that did not reach the disk is a cook
    // that silently produced nothing under a path the runtime will ask for.
    enum class TextureCookOutcome
    {
        Cooked,
        Fresh,
        Failed,
        Unwritten,
    };

    struct TextureCookResult
    {
        Common::UUID       Handle;
        TextureCookOutcome Outcome = TextureCookOutcome::Failed;
    };

    struct LooseTextureCookStats
    {
        size_t Cooked    = 0;
        size_t Fresh     = 0;
        size_t Failed    = 0;
        size_t Unwritten = 0;
    };

    // THE DIRECTORIES WHOSE IMAGE FILES ARE COOKED WITHOUT A MESH ASKING FOR THEM — one list, read by the
    // editor's startup cook, its "Rebuild Cooked Assets" command AND the packager. It used to be spelled at
    // each editor site as a pair of calls, and the packager did not spell it at all: a package carried only
    // the `.tex` files some editor session had happened to leave on the disk, which in CI was the one
    // committed checker texture. `Assets/Textures/` holds the loose textures and the sky panoramas
    // (`Textures/HDR/`); `Assets/Meshes/` holds the images beside a mesh, which the mesh cook writes only
    // when the mesh itself is re-imported.
    std::array<const std::filesystem::path*, 2> LooseTextureRoots();

    // Every texture source under `LooseTextureRoots()` — the files `TextureImporter::CookLooseTextures`
    // cooks, in the order it cooks them. Public so a gate can ask "is this referenced texture one the
    // cook reaches" of the cook's own enumeration rather than of a copy of its filter.
    std::vector<std::filesystem::path> LooseTextureSources();

    class TextureImporter
    {
    public:
        Common::UUID Import( const std::filesystem::path& path );

        // `Import`, plus what the call did. Freshness is decided exactly as `Import` decides it — from the
        // source's bytes, the cook signature and the two identity fields (see the .cpp) — so an up-to-date
        // `.tex` is `Fresh` and is not rewritten.
        TextureCookResult Cook( const std::filesystem::path& path );

        // Cooks every texture source under `LooseTextureRoots()`. THERE IS NO `force` PARAMETER, and
        // there must not be one: freshness is a fact about the source's bytes, so "force" would mean
        // "re-cook things that are already correct".
        // @p progress names each source as it is cooked (the splash's item line).
        LooseTextureCookStats CookLooseTextures( const Assets::ItemProgress& progress = {} );

        // The cooked metadata (.tex) path a given source texture is cooked into (Cooked/Textures/...).
        // Public so callers can CreateAsset<TextureAsset>() on it right after Import().
        static std::filesystem::path CookedMetaPath( const std::filesystem::path& source );

    private:
        std::unordered_map<std::string, Common::UUID> m_Cache;
    };
} // namespace Desert::Editor