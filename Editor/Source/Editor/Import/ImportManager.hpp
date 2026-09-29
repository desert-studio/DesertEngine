#pragma once

#include <Engine/Assets/AssetGuidRef.hpp>
#include <memory>
#include <unordered_map>
#include "BackgroundCook.hpp"
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
        // The verdict is what the background startup cook (EditorLayer::DrainBackgroundCook) acts on.
        CookVerdict Import( const std::filesystem::path& path, bool force = false );
        // The mesh sources under `root` the bulk cook reaches (`.blend` excluded: a headless Blender run is
        // imported on demand only).
        static std::vector<std::filesystem::path> MeshSources( const std::filesystem::path& root );
        void         ImportAllFromDirectory( const std::filesystem::path& root, bool force = false );
        Common::UUID ImportTexture( const std::filesystem::path& path );

        // Publishes the import templates every importer chooses among: the loaded ShaderAssets of `manager`
        // whose manifest declares an Import block (the registry, not a directory scan). Call after the shaders
        // load; a material import before it is refused. Returns how many templates were published.
        static std::size_t PublishImportTemplates( const Assets::AssetManager& manager );

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
        [[nodiscard]] Common::BoolResultStr
        SerializeMaterialAsset( const ImportedMaterial& material, const std::filesystem::path& sourcePath,
                                const std::optional<Assets::AssetGuidRef>& previewMesh );

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