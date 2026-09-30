#pragma once

#include <Engine/Assets/MeshSourceAsset.hpp>
#include <Engine/Assets/AssetGuidRef.hpp>
#include <memory>
#include <unordered_map>
#include "BackgroundCook.hpp"
#include "IAssetImporter.hpp"
#include "MaterialImportContract.hpp"
#include "ImportResult.hpp"
#include "TextureImporter.hpp"

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/ItemProgress.hpp>

namespace Desert::Assets
{
    class TextureAsset;
}

namespace Desert::Editor
{
    // What an import with options wrote: its verdict, and every asset file derived from the source that now
    // holds new content - each mesh (a static node mesh, the combined mesh, a `.skmesh`), the `.skeleton` and
    // every `.anim` - so the caller refreshes whoever uses them (Reimport, UE's FReimportManager ->
    // PostReimport, which reimports the mesh, its skeleton and its animations together). A partly failed import
    // still lists what it did write.
    struct ImportOutcome
    {
        CookVerdict                        Verdict = CookVerdict::Failed;
        std::vector<std::filesystem::path> WrittenMeshes;
        std::vector<std::filesystem::path> WrittenSkeletons;
        std::vector<std::filesystem::path> WrittenClips;
    };

    // THE NUMBER A WRITTEN FILE IS KNOWN BY, AS EVERY REFERENCE HOLDS IT: its content-registry row's handle. A
    // file with a header GUID (a `.skmesh`, whose GUID a reimport keeps) is known by the GUID's fold, NOT by its
    // path's hash, and a component names it by the row the picker gave it - so a reimport that refreshed the
    // loaded mesh under `AssetHandle::FromCookedPath` missed every skinned mesh: the service kept the old build
    // and Details read its Approx Size, the skin drew the old vertices with the rescaled binds (THM1l-b22, live
    // on Fox.glb at Uniform Scale 10). A written file with no row is an error: the write registers it
    // (CookedJsonWrite.hpp, ContentRegistry::NoteFile), so no row means the write did not happen.
    Common::ResultStr<Assets::AssetHandle> LoadedHandleOf( const std::filesystem::path& written );

    class ImportManager
    {
    public:
        ImportManager();

        // force = re-cook even if an up-to-date cooked output already exists (Rebuild Cooked Assets).
        // The verdict is what the background startup cook (EditorLayer::DrainBackgroundCook) acts on.
        CookVerdict Import( const std::filesystem::path& path, bool force = false );
        // THE IMPORT WITH OPTIONS (THM1l; UE: the FBX Import Options window's Import, and the asset's Reimport).
        // Always imports (a changed option must reach the meshes even when the file's bytes did not change);
        // @p settings are written into the source's import record, the options' one home, which every later
        // Import reads.
        ImportOutcome ImportWithSettings( const std::filesystem::path&        path,
                                          const Assets::SourceImportSettings& settings );
        // What @p path holds (the Import Options window's title and sections); an error for a file no importer
        // takes or one that does not parse.
        Common::ResultStr<ImportContentKind> ProbeContent( const std::filesystem::path& path );
        // The mesh sources under `root` the bulk cook reaches (`.blend` excluded: a headless Blender run is
        // imported on demand only).
        static std::vector<std::filesystem::path> MeshSources( const std::filesystem::path& root );
        void         ImportAllFromDirectory( const std::filesystem::path& root, bool force = false );
        Common::UUID ImportTexture( const std::filesystem::path& path );

        // Publishes the import templates every importer chooses among: the loaded ShaderAssets of `manager`
        // whose manifest declares an Import block (the registry, not a directory scan). Call after the shaders
        // load; a material import before it is refused. Returns how many templates were published.
        static std::size_t PublishImportTemplates( const Assets::AssetManager& manager );
        // Publishes @p templates as they are: the registry overload above reads them from the loaded shaders and
        // lands here, and a caller without a renderer (the import suites) reads the shipped shader files with
        // ReadImportTemplate and hands them over. Replaces whatever was published; returns the count.
        static std::size_t PublishImportTemplates( std::vector<ImportTemplate> templates );

        /// Imports every loose image under `LooseTextureRoots()` that has no `.detex` yet; returns the count.
        /// See the definition for why the mesh scan could not do this and why there is no `force`.
        /// @p progress names each source as it is reached (the splash's item line).
        size_t ImportLooseTextures( const Assets::ItemProgress& progress = {} );

        // Import a source texture into its `.detex` asset and create its TextureAsset in @p mgr; null on failure.
        // The cook stops at the asset: registering it with the runtime's TextureService is the caller's
        // (TextureDnD::ImportAndRegister), so the import layer links without the renderer (THM1l-b16: the
        // SkinnedImport suite runs ImportManager on a mock).
        std::shared_ptr<Assets::TextureAsset> ImportTextureAsset( Assets::AssetManager&        mgr,
                                                                  const std::filesystem::path& source );

    private:
        // EVERY ONE OF THESE ANSWERS WHETHER THE COOKED FILE IS ON THE DISK (Д35). They were `void`, and
        // the write underneath them was a `std::ofstream` nobody checked after the insertion — so a full
        // disk or a read-only Cooked/ produced an import that looked exactly like a successful one, and
        // the missing `.stmesh` surfaced later as an asset that would not resolve. The write itself is
        // WriteCookedJson (Editor/Import/CookedJsonWrite.hpp), which closes before it decides.
        [[nodiscard]] ImportOutcome ImportParsed( const std::filesystem::path&        path,
                                                  const Assets::SourceImportSettings& settings );
        // @p written receives every mesh, skeleton and clip file actually written, also when a later write fails.
        [[nodiscard]] Common::BoolResultStr CreateAssetsFromImport( const ImportResult&                 result,
                                                                    const std::filesystem::path&        sourcePath,
                                                                    const Assets::SourceImportSettings& settings,
                                                                    ImportOutcome&                      written );

    private:
        [[nodiscard]] Common::BoolResultStr
        SerializeMeshAsset( const Desert::Assets::Serialization::MeshAssetData& data,
                            const std::filesystem::path&                        sourcePath );

        // Success ALSO means "a .demat was already there and was deliberately kept" — re-import must not
        // clobber the artist's edits, so not writing is the correct outcome, not a failure to write.
        [[nodiscard]] Common::BoolResultStr SerializeMaterialAsset( const ImportedMaterial&      material,
                                                                    const std::filesystem::path& sourcePath );

        // The GUID the written .skeleton states (kept from the file it replaces, minted for a new one).
        [[nodiscard]] Common::ResultStr<Common::Content::AssetGuid>
        SerializeSkeletonAsset( const Desert::Assets::Serialization::SkeletonAssetData& data,
                                const std::filesystem::path&                            sourcePath );

        [[nodiscard]] static Common::BoolResultStr
        SerializeAnimationAsset( const Desert::Animation::AnimationClip& clip,
                                 const std::filesystem::path&            sourcePath );

    private:
        std::unordered_map<std::string, std::unique_ptr<IAssetImporter>> m_Importers;
        std::unique_ptr<TextureImporter>                                 m_TextureImporter;
    };
} // namespace Desert::Editor