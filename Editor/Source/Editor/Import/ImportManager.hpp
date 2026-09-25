#pragma once

#include <memory>
#include <unordered_map>
#include "IAssetImporter.hpp"
#include "ImportResult.hpp"
#include "TextureImporter.hpp"

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/ItemProgress.hpp>

namespace Desert::Editor
{
    class ImportManager
    {
    public:
        ImportManager();

        // force = re-cook even if an up-to-date cooked output already exists (Rebuild Cooked Assets).
        void         Import( const std::filesystem::path& path, bool force = false );
        void         ImportAllFromDirectory( const std::filesystem::path& root, bool force = false );
        Common::UUID ImportTexture( const std::filesystem::path& path );

        /// Imports every loose image under `LooseTextureRoots()` that has no `.detex` yet; returns the count.
        /// See the definition for why the mesh scan could not do this and why there is no `force`.
        /// @p progress names each source as it is reached (the splash's item line).
        size_t ImportLooseTextures( const Assets::ItemProgress& progress = {} );

        // Import a source texture into its `.detex` asset, create+register a TextureAsset, and return its
        // handle (the same handle TextureService keys by). Returns a zero handle on failure. Drives the
        // import-on-demand drag-drop path (see Editor::TextureDnD).
        Assets::AssetHandle ImportAndRegisterTexture( Assets::AssetManager&         mgr,
                                                      const std::filesystem::path& source );

    private:
        // EVERY ONE OF THESE ANSWERS WHETHER THE COOKED FILE IS ON THE DISK (Д35). They were `void`, and
        // the write underneath them was a `std::ofstream` nobody checked after the insertion — so a full
        // disk or a read-only Cooked/ produced an import that looked exactly like a successful one, and
        // the missing `.stmesh` surfaced later as an asset that would not resolve. The write itself is
        // WriteCookedJson (Editor/Import/CookedJsonWrite.hpp), which closes before it decides.
        [[nodiscard]] Common::BoolResultStr CreateAssetsFromImport( const ImportResult&          result,
                                                                    const std::filesystem::path& sourcePath );

    private:
        [[nodiscard]] Common::BoolResultStr
        SerializeMeshAsset( const Desert::Assets::Serialization::MeshAssetData& data,
                            const std::filesystem::path&                        sourcePath );

        // Success ALSO means "a .demat was already there and was deliberately kept" — re-import must not
        // clobber the artist's edits, so not writing is the correct outcome, not a failure to write.
        [[nodiscard]] Common::BoolResultStr SerializeMaterialAsset( const ImportedMaterial&      material,
                                                                    const std::filesystem::path& sourcePath );

        [[nodiscard]] Common::BoolResultStr
        SerializeSkeletonAsset( const Desert::Assets::Serialization::SkeletonAssetData& data,
                                const std::filesystem::path&                            sourcePath );

        [[nodiscard]] Common::BoolResultStr
        SerializeAnimationAsset( const Desert::Assets::Serialization::AnimationAssetData& data,
                                 const std::filesystem::path&                             sourcePath );

    private:
        std::unordered_map<std::string, std::unique_ptr<IAssetImporter>> m_Importers;
        std::unique_ptr<TextureImporter>                                 m_TextureImporter;
    };
} // namespace Desert::Editor