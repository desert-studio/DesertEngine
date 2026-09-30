#include "ImportManager.hpp"
#include <Engine/Assets/TextAssetHeaderIdentity.hpp>

#include <Common/Core/Serialization/GlmReflection.hpp>

#include "Assimp/AssimpImporter.hpp"
#include "Blend/BlendImporter.hpp"
#include "CookPaths.hpp"
#include "CookedJsonWrite.hpp"
#include "MaterialAdoption.hpp"
#include "MaterialImportContract.hpp"
#include "TextureChannelPack.hpp"

#include <Engine/Assets/Serialization/MeshBinary.hpp>
#include <Engine/Assets/Serialization/Skeleton.hpp>
#include <Common/Content/MeshBinaryHeader.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include "ImportedMeshAsset.hpp"
#include "MeshDeriver.hpp"
#include "NodeMeshSplit.hpp"
#include "SourceToEngine.hpp"
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/Serialization/ImportRecord.hpp>
#include <Engine/Assets/MeshDerivedData.hpp>

#include <Common/Core/Constants.hpp>

#include <Engine/Assets/MaterialFormat.hpp>
#include <Engine/Assets/TextureAsset.hpp>
#include <Engine/Assets/Shader/ShaderAsset.hpp>
#include <Engine/Assets/TextureSourceAsset.hpp>

#include <Common/Core/JobSystem.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>

namespace Desert::Editor
{

    static std::filesystem::path SkinnedAssetPath( const std::filesystem::path& sourcePath,
                                                   const std::string&           suffix )
    {
        // Path formula is shared (CookPaths::SkinnedAsset, beside the source); this wrapper also ensures the
        // dir exists for writing.
        const auto result = Editor::CookPaths::SkinnedAsset( sourcePath, suffix );
        std::filesystem::create_directories( result.parent_path() );
        return result;
    }

    Common::ResultStr<Assets::AssetHandle> LoadedHandleOf( const std::filesystem::path& written )
    {
        const auto kind = Assets::ContentRegistry::KindForFile( written );
        if ( !kind )
            return Common::MakeFormattedError<Assets::AssetHandle>( "'{}' is no content kind the registry keeps",
                                                                    written.generic_string() );
        const auto row = Assets::ContentRegistry::RowAtPath( *kind, written );
        if ( !row )
            return Common::MakeFormattedError<Assets::AssetHandle>(
                 "'{}' was written and has no content-registry row", written.generic_string() );
        return Common::MakeSuccess( row->Handle );
    }

    ImportManager::ImportManager()
    {
        m_TextureImporter     = std::make_unique<TextureImporter>();
        m_Importers[".fbx"]   = std::make_unique<AssimpImporter>();
        m_Importers[".obj"]   = std::make_unique<AssimpImporter>();
        m_Importers[".gltf"]  = std::make_unique<AssimpImporter>();
        m_Importers[".glb"]   = std::make_unique<AssimpImporter>();
        // .blend is converted to FBX via Blender headless, then run through the Assimp path (see BlendImporter).
        m_Importers[".blend"] = std::make_unique<BlendImporter>();
    }

    namespace
    {
        // A skinned import is current when its mesh exists and its rig states the hash of the source's CURRENT
        // bytes (AF8b, the texture IMPT rule of AF7). Bytes, not times: the skinned assets are authored content
        // committed beside their source, and a fresh checkout writes both in no particular order - an mtime
        // test re-imported them at the first editor start and left the tree dirty.
        bool SkinnedImportIsFresh( const std::filesystem::path& source )
        {
            std::error_code ec;
            if ( !std::filesystem::exists( CookPaths::SkinnedAsset( source, ".skmesh" ), ec ) )
                return false;
            const auto text = Common::Utils::FileSystem::ReadFileContentIfExists(
                 CookPaths::SkinnedAsset( source, ".skeleton" ) );
            if ( !text )
                return false;
            const auto& content = text.GetValue();
            if ( !content.has_value() )
                return false;
            const auto rig = Assets::Serialization::ReadSkeletonJson( content.value() );
            if ( !rig )
                return false;
            const auto& import = rig.GetValue().Import;
            if ( !import.has_value() )
                return false;
            const auto hash = Assets::HashMeshSourceFile( source );
            return hash && import.value().SourceHash == hash.GetValue();
        }
    } // namespace

    CookVerdict ImportManager::Import( const std::filesystem::path& path, bool force )
    {
        auto ext = path.extension().string();
        std::transform( ext.begin(), ext.end(), ext.begin(), ::tolower );

        if ( !m_Importers.contains( ext ) )
            return CookVerdict::NotCookable;

        // Skip the expensive Assimp re-parse (+ its texture/material re-cook) when the mesh output is
        // current. A source produces either a static mesh envelope in the DDC (fresh by its IMPT content
        // hash - AF4h moved that envelope out from beside the source) or skinned assets beside the
        // source (fresh by the source hash their rig states, AF8b), so accept either. `force` (Rebuild Cooked
        // Assets) bypasses this. A mesh EDITED in the editor (P9b) is up to date whatever its source's bytes say:
        // only an explicit re-import may replace the edit (UE: a changed .fbx is offered for re-import, never
        // re-imported behind the user's back), and that re-import says so (RemoveBesideSourceFile).
        if ( !force &&
             ( ImportedMeshAssetIsCurrent( path ) ||
               Assets::IsEditedImportedMesh( CookPaths::MeshAsset( path ) ) || SkinnedImportIsFresh( path ) ) )
            return CookVerdict::UpToDate;

        // The options the source was imported with last (its record), UE's defaults on a first import.
        const auto settings = Assets::Serialization::ReadImportRecordSettings( path );
        if ( !settings )
        {
            LOG_ERROR( "[Import] '{}' was not imported: {}", path.string(), settings.GetError() );
            return CookVerdict::Failed;
        }
        return ImportParsed( path, settings.GetValue() ).Verdict;
    }

    ImportOutcome ImportManager::ImportWithSettings( const std::filesystem::path&        path,
                                                     const Assets::SourceImportSettings& settings )
    {
        auto ext = path.extension().string();
        std::transform( ext.begin(), ext.end(), ext.begin(), ::tolower );
        if ( !m_Importers.contains( ext ) )
            return { CookVerdict::NotCookable, {}, {}, {} };
        return ImportParsed( path, settings );
    }

    Common::ResultStr<ImportContentKind> ImportManager::ProbeContent( const std::filesystem::path& path )
    {
        auto ext = path.extension().string();
        std::transform( ext.begin(), ext.end(), ext.begin(), ::tolower );
        if ( !m_Importers.contains( ext ) )
            return Common::MakeFormattedError<ImportContentKind>( "'{}': no importer takes '{}' files",
                                                                  path.string(), ext );
        return m_Importers[ext]->Probe( path );
    }

    ImportOutcome ImportManager::ImportParsed( const std::filesystem::path&        path,
                                               const Assets::SourceImportSettings& settings )
    {
        auto ext = path.extension().string();
        std::transform( ext.begin(), ext.end(), ext.begin(), ::tolower );
        auto result = m_Importers[ext]->Import( path, *this );
        // The cook's verdict stops HERE, at a log line naming the file and the reason. It has nowhere
        // further to go and that is deliberate rather than overlooked: this function is void because
        // both of its callers are fire-and-forget — the boot scan (ImportAllFromDirectory, on
        // JobSystem workers) and drag-and-drop. Widening it into a result would oblige every one of
        // those to grow an answer nobody is waiting for. What Д31-D asked for is that a cooked file
        // that was never written stops being INDISTINGUISHABLE from one that was; it now is.
        ImportOutcome outcome{ CookVerdict::Cooked, {}, {}, {} };
        if ( const auto cooked = CreateAssetsFromImport( result, path, settings, outcome ); !cooked )
        {
            LOG_ERROR( "[Import] '{}' was parsed but its cooked output is incomplete: {}", path.string(),
                       cooked.GetError() );
            outcome.Verdict = CookVerdict::Failed;
        }
        return outcome;
    }

    std::vector<std::filesystem::path> ImportManager::MeshSources( const std::filesystem::path& root )
    {
        namespace fs = std::filesystem;

        std::vector<fs::path> files;
        std::error_code       ec;
        if ( !fs::exists( root, ec ) ) // tolerate a missing source dir (e.g. clean/from-scratch project)
            return files;

        const ImportManager probe; // the importer table is the one filter; `.blend` is left to on-demand
        for ( const auto& entry : fs::recursive_directory_iterator( root, ec ) )
        {
            if ( !entry.is_regular_file() )
                continue;

            std::string ext = entry.path().extension().string();
            std::transform( ext.begin(), ext.end(), ext.begin(), ::tolower );

            // .blend is converted via a (potentially multi-minute) headless Blender run — far too heavy to do
            // for every file during a bulk scan. It's imported ON DEMAND instead (drag-drop goes through
            // AsyncMeshLoader on a worker thread), so the editor never freezes on it.
            if ( ext == ".blend" )
                continue;

            if ( probe.m_Importers.contains( ext ) )
                files.push_back( entry.path() );
        }
        return files;
    }

    void ImportManager::ImportAllFromDirectory( const std::filesystem::path& root, bool force )
    {
        namespace fs = std::filesystem;

        // Gather first, cook in PARALLEL after: each source file is an independent CPU+disk job (that is
        // exactly what AsyncMeshLoader relies on for single files). Each worker thread cooks on its OWN
        // ImportManager (Assimp importers are not reentrant); shared cooked-texture writes are serialized
        // inside TextureImporter.
        const std::vector<fs::path> files = MeshSources( root );
        if ( files.empty() )
            return;

        if ( files.size() == 1 )
        {
            (void)Import( files.front(), force );
            return;
        }

        const auto started = std::chrono::steady_clock::now();
        Common::JobSystem::Get().ParallelFor( files.size(),
                                              [&]( size_t i )
                                              {
                                                  // One cooker per worker thread, reused across files.
                                                  thread_local ImportManager s_ThreadImporter;
                                                  (void)s_ThreadImporter.Import( files[i], force );
                                              } );
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started )
                             .count();
        LOG_INFO( "[Import] {} source file(s) checked/cooked in {} ms ({} workers)", files.size(), ms,
                  Common::JobSystem::Get().WorkerCount() );
    }

    // EVERY PART IS ATTEMPTED, AND THE FIRST FAILURE IS THE ONE RETURNED. Stopping at the first would
    // make one unwritable material hide a mesh that could have been cooked. Only the first reason
    // travels up — the caller acts on "this cook is incomplete", not on the list — and every reason
    // names its own file, so the one that arrives is enough to find the cause.
    Common::BoolResultStr ImportManager::CreateAssetsFromImport( const ImportResult&                 result,
                                                                 const std::filesystem::path&        sourcePath,
                                                                 const Assets::SourceImportSettings& settings,
                                                                 ImportOutcome&                      written )
    {
        std::vector<std::filesystem::path>& writtenMeshes = written.WrittenMeshes;
        std::string firstFailure;
        const auto  record = [&firstFailure]( const Common::BoolResultStr& outcome )
        {
            if ( !outcome && firstFailure.empty() )
                firstFailure = outcome.GetError();
        };

        // Materials are resolved BEFORE the mesh is written: a .demat already on disk keeps its GUID, and the
        // submeshes are re-pointed at it, so the cooked mesh names the material the project actually holds.
        ImportResult resolved = result;
        if ( const auto adopted = MaterialAdoption::AdoptExistingMaterials( resolved, sourcePath ); !adopted )
            return Common::MakeFormattedError<bool>( "'{}' material adoption refused: {}", sourcePath.string(),
                                                     adopted.GetError() );

        // IMPORT OPTIONS REACH EVERY MESH OF THE FILE (UE FBX Import Options): a static mesh carries them in its
        // source and the deriver applies them; a skinned mesh, its skeleton and its clips are written in render
        // form, so Uniform Scale / Up Axis are baked into all three here (SourceToEngine.hpp) and LOD Generate
        // simplifies the skinned sections as the deriver does the static ones. A file with no static mesh
        // (skinned, or skeleton + clips only) is the case: the static path never sees the skeleton.
        const bool staticMesh = resolved.Mesh && !resolved.Mesh->IsSkinned;
        // THE RECORD FOR EVERY KIND (RecordImport): the static writer records the import itself; a skinned file,
        // or one with a skeleton and clips only, is recorded here (the box the options give it). A
        // parse that produced nothing (a failed .blend conversion) is no import and leaves no record.
        if ( !staticMesh )
        {
            // What the file imports as: a skinned mesh, else the skeleton, else clips only.
            Common::Content::ContentKind kind = Common::Content::ContentKind::Animation;
            if ( resolved.Mesh )
                kind = Common::Content::ContentKind::SkinnedMesh;
            else if ( resolved.Skeleton )
                kind = Common::Content::ContentKind::Skeleton;
            if ( resolved.Mesh || resolved.Skeleton || !resolved.Animations.empty() )
                record( RecordImport( sourcePath, kind, resolved.Mesh ? &resolved.Mesh.value() : nullptr,
                                      settings ) );
            // The file's unit was logged at the parse ("geometry scaled by <cm per unit>"); the record's options
            // are the second factor, baked here, and say so - a reimport at Scale 10 read "scaled by 100" alone.
            LOG_INFO( "[Import] '{}': Uniform Scale {} and Up Axis {} (import options) baked into the mesh, the "
                      "rig and "
                      "{} clip(s)",
                      sourcePath.generic_string(), settings.Mesh.UniformScale,
                      settings.Mesh.UpAxis == Assets::MeshSourceUpAxis::Z   ? "Z"
                      : settings.Mesh.UpAxis == Assets::MeshSourceUpAxis::Y ? "Y"
                                                                            : "from the file",
                      resolved.Animations.size() );
            ApplySourceToEngine( settings.Mesh, resolved.Mesh ? &resolved.Mesh.value() : nullptr,
                                 resolved.Skeleton ? &resolved.Skeleton.value() : nullptr, resolved.Animations );
        }
        // THE RIG AND ITS CLIPS ENTER THE REGISTRY BEFORE THE MESH THAT NAMES THEM (UE creates the USkeleton
        // before the USkeletalMesh). Each write is registered the moment it exists (CookedJsonWrite.hpp), and a
        // skinned mesh is resolved through the Skeleton row its signature names (MeshService::Arrived): with the
        // mesh written first, a draw or thumbnail between the two writes failed the mesh for the session - live on
        // Fox.glb, "no Skeleton row of the content registry states it".
        if ( resolved.Skeleton )
        {
            const auto serialized = SerializeSkeletonAsset( resolved.Skeleton.value(), sourcePath );
            if ( serialized )
                written.WrittenSkeletons.push_back( SkinnedAssetPath( sourcePath, ".skeleton" ) );
            record( serialized );
        }

        for ( const auto& anim : resolved.Animations )
        {
            const auto serialized = SerializeAnimationAsset( anim, sourcePath );
            if ( serialized )
                written.WrittenClips.push_back(
                     SkinnedAssetPath( sourcePath, std::format( "_{}.anim", anim.Name ) ) );
            record( serialized );
        }

        if ( resolved.Mesh && resolved.Mesh->IsSkinned )
        {
            if ( settings.Mesh.LodPolicy == Assets::MeshLodPolicy::Generate )
                BakeMeshLODs( resolved.Mesh.value() );
            const auto serialized = SerializeMeshAsset( resolved.Mesh.value(), sourcePath );
            if ( serialized )
                writtenMeshes.push_back( SkinnedAssetPath( sourcePath, ".skmesh" ) );
            record( serialized );
        }
        else if ( resolved.Mesh )
        {
            // The static mesh is a source asset beside its file (ImportedMeshAsset.hpp); its slots are named
            // from the materials resolved above, whose GUIDs the submeshes now carry.
            std::vector<Assets::MeshMaterialSlot> named;
            named.reserve( resolved.Materials.size() );
            for ( const auto& material : resolved.Materials )
                named.push_back( { material.Name, material.Guid } );
            // COMBINE MESHES OFF (UE's default): every mesh-bearing node becomes its own static mesh and NO
            // combined one is written (NodeMeshSplit.hpp WriteStaticMeshImport); a single-node source, or Combine
            // Meshes on, is the one combined mesh.
            auto written = WriteStaticMeshImport( resolved.Mesh.value(), resolved.SubmeshNodes, named, sourcePath,
                                                  settings );
            if ( !written )
                record( Common::MakeError<bool>( written.GetError() ) );
            else
            {
                for ( const auto& [node, path] : written.GetValue() )
                {
                    Assets::ContentRegistry::NoteFile( path );
                    writtenMeshes.push_back( path );
                }
            }
        }

        // NO PREVIEW MESH FROM THE IMPORT (owner, THM1j): an imported material's thumbnail is the ball, as in UE,
        // masked materials included (the mesh path's alpha discard cuts the ball). PreviewMesh stays the manual
        // "Thumbnail Mesh" setting of the Material Editor; the pack's tufts are shown by the node meshes' own
        // thumbnails.
        for ( const auto& material : resolved.Materials )
            record( SerializeMaterialAsset( material, sourcePath ) );

        if ( !firstFailure.empty() )
            return Common::MakeError<bool>( firstFailure );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr
    ImportManager::SerializeMeshAsset( const Desert::Assets::Serialization::MeshAssetData& dataIn,
                                       const std::filesystem::path&                        sourcePath )
    {
        // SKINNED ONLY: a static mesh is written by WriteImportedMeshAsset beside its source, and its LOD
        // chain is derived on load (BuildMeshPlatformData). Skinned meshes never had LODs folded or baked.
        if ( !dataIn.IsSkinned )
            return Common::MakeFormattedError<bool>(
                 "'{}': SerializeMeshAsset writes only skinned meshes; a static mesh is a MeshSourceAsset",
                 sourcePath.string() );
        Desert::Assets::Serialization::MeshAssetData data       = dataIn;
        const std::filesystem::path                  cookedPath = SkinnedAssetPath( sourcePath, ".skmesh" );

        // THE ONE PLACE A COOKED MESH IS WRITTEN, and since B11 it writes the binary container rather
        // than JSON. The sibling cooked kinds beside this one (.skeleton, .anim, .demat, .tex metadata)
        // are unchanged: they are kilobytes of structure, not megabytes of floats, and the argument
        // that moved this one does not reach them.
        // A RE-IMPORT KEEPS THE MESH'S IDENTITY (UE keeps a package's GUID on reimport): scenes name the
        // mesh by this GUID, so minting a new one would orphan every reference to the file being replaced.
        data.Guid = Common::Content::AssetGuid::Generate();
        // First import: no file yet, so absence is an answer here, not an error.
        if ( const auto prefix = Common::Utils::FileSystem::ReadFileContentPrefixIfExists(
                  cookedPath, Common::Content::kMeshBinaryPrefixV3 ) )
        {
            const auto& previous = prefix.GetValue();
            if ( previous.has_value() )
                if ( const auto kept = Common::Content::ReadMeshHeaderGuid( previous.value() );
                     kept.has_value() && !kept.value().IsNull() )
                    data.Guid = kept.value();
        }
        return WriteCookedBytes( Desert::Assets::Serialization::EncodeMeshBinary( data ), cookedPath,
                                 Desert::Assets::Serialization::MeshDataBounds( data ) );
    }

    Common::BoolResultStr
    ImportManager::SerializeSkeletonAsset( const Desert::Assets::Serialization::SkeletonAssetData& data,
                                           const std::filesystem::path&                            sourcePath )
    {
        auto cookedPath = SkinnedAssetPath( sourcePath, ".skeleton" );
        // A RE-IMPORT KEEPS THE RIG'S IDENTITY (T7e, SKEL 1): the GUID of the file being replaced, minted only
        // for a new one - retargets and meshes name the rig, and a fresh GUID would orphan them.
        auto stamped   = data;
        stamped.Header = Assets::HeaderKeepingFileGuid( cookedPath, Common::Content::ContentKind::Skeleton,
                                                        Assets::Serialization::SkeletonTextSubsystems() );
        // The rig carries the import's source hash: SkinnedImportIsFresh reads it back.
        const auto hash = Assets::HashMeshSourceFile( sourcePath );
        if ( !hash )
            return Common::MakeError<bool>( hash.GetError() );
        stamped.Import =
             Assets::Serialization::SkeletonImportInfo{ sourcePath.filename().generic_string(), hash.GetValue() };
        return WriteCookedJson( stamped, cookedPath );
    }

    Common::BoolResultStr
    ImportManager::SerializeAnimationAsset( const Desert::Assets::Serialization::AnimationAssetData& data,
                                            const std::filesystem::path&                             sourcePath )
    {
        auto cookedPath = SkinnedAssetPath( sourcePath, "_" + data.Name + ".anim" );
        // A RE-IMPORT KEEPS THE CLIP'S IDENTITY (T7e, ANIM 4), as the rig's above: sequencer tracks and anim
        // graphs name the clip, and a fresh GUID would orphan them.
        auto stamped   = data;
        stamped.Header = Assets::HeaderKeepingFileGuid( cookedPath, Common::Content::ContentKind::Animation,
                                                        Assets::Serialization::AnimationTextSubsystems() );
        return WriteCookedJson( stamped, cookedPath );
    }

    namespace
    {
        // THE IMPORT TEMPLATES ARE THE ASSET REGISTRY'S SHADERS (MAT1b), not a second scan of the shader
        // directory: whatever the editor loaded as a ShaderAsset, and only that, can take an imported material.
        // Published once the shaders are loaded (EditorLayer, after CompileEngineShaders) and read by every
        // importer — the background cook's per-thread ones included — so the snapshot is swapped under a lock.
        std::mutex                                         s_TemplatesMutex;
        std::shared_ptr<const std::vector<ImportTemplate>> s_Templates;

        std::shared_ptr<const std::vector<ImportTemplate>> ImportTemplates()
        {
            const std::scoped_lock lock( s_TemplatesMutex );
            return s_Templates;
        }
    } // namespace

    std::size_t ImportManager::PublishImportTemplates( const Assets::AssetManager& manager )
    {
        const std::filesystem::path resources =
             std::filesystem::path( Common::Constants::Path::SHADERDIR_PATH ).parent_path().parent_path();
        std::vector<ImportTemplate> templates;
        for ( const auto& [handle, shader] : manager.FindAllByType<Assets::ShaderAsset>() )
        {
            if ( !shader->IsReadyForUse() )
                continue;
            const std::filesystem::path& file = shader->GetMetadata().Filepath;
            auto                         read = ReadImportTemplate(
                 shader->GetShaderContent(),
                 std::format( "engine:{}", std::filesystem::relative( file, resources ).generic_string() ) );
            // A shader the readers refuse cannot take a material, and saying so is the refusal.
            if ( !read )
            {
                LOG_ERROR( "[Import] {}", read.GetError() );
            }
            else if ( read.GetValue().Manifest.DeclaresImport )
                templates.push_back( read.ExtractValue() );
        }
        return PublishImportTemplates( std::move( templates ) );
    }

    std::size_t ImportManager::PublishImportTemplates( std::vector<ImportTemplate> templates )
    {
        auto published          = std::make_shared<const std::vector<ImportTemplate>>( std::move( templates ) );
        const std::size_t count = published->size();
        const std::scoped_lock lock( s_TemplatesMutex );
        s_Templates = std::move( published );
        return count;
    }

    Common::BoolResultStr
    ImportManager::SerializeMaterialAsset( const ImportedMaterial&                    material,
                                           const std::filesystem::path&               sourcePath )
    {
        // Imported materials are EDITABLE CONTENT, not cooked intermediates -> write them into the content
        // tree at Resources/Assets/Materials/<meshRelativeId>/<materialName>.demat (browsable + editable in
        // the asset browser, reusable), like UE.
        // Meaningful name, NO handle in it (stable identity lives in the file: its header GUID).
        // Unified .demat schema (legacy ".mat" cooker output is gone; SurfaceMaterialAsset::Load still READS old).
        //
        // The per-mesh subfolder is CookPaths::MaterialFolder and not `MATERIAL_PATH / stem` spelled here.
        // The stem alone put two same-named meshes from different folders in one folder, where the
        // "only write if MISSING" rule below turns a collision into a silent adoption: the second mesh's
        // materials are never written and it inherits the first mesh's instead.
        const std::filesystem::path path = MaterialAdoption::MaterialAssetPath( sourcePath, material.Name );

        // Only write if MISSING: re-importing a mesh must NOT clobber the user's edits to its material (UE
        // behaviour — re-import updates geometry, keeps the material asset). Delete the .demat to regenerate.
        // The kept file's GUID was already adopted into `material` and the submeshes (MaterialAdoption).
        std::error_code ec;
        if ( std::filesystem::exists( path, ec ) )
            return BOOLSUCCESS; // deliberately kept, not a failure to write
        // THE TEMPLATE CHOOSES ITSELF (MAT1b): every shipped shader with an Import block is a candidate, the
        // core picks the one whose Requires the source satisfies most, and that template's rows say which
        // source key fills which Property. No taker is a refusal naming the material and the file.
        const auto published = ImportTemplates();
        if ( !published )
            return Common::MakeError<bool>( std::format(
                 "material '{}' in '{}': no import templates are published (the shaders are not loaded yet; "
                 "ImportManager::PublishImportTemplates runs after they are)",
                 material.Name, sourcePath.generic_string() ) );
        const std::vector<ImportTemplate>& templates = *published;
        const auto choice = ChooseImportTemplate( material.Source, templates, sourcePath.generic_string() );
        if ( !choice )
            return Common::MakeError<bool>( choice.GetError() );
        const ImportTemplate& chosen = templates[choice.GetValue()];
        const TemplateFill    fill   = FillFromTemplate( material.Source, chosen );
        for ( const std::string& key : fill.UnreadKeys )
            LOG_WARN( "[Import][Material] '{}' in '{}': template '{}' has no Import row for '{}', so it is NOT "
                      "imported",
                      material.Name, sourcePath.generic_string(), chosen.ShaderName, key );

        Assets::MaterialData data = ImportedMaterialDocument( chosen, fill );
        for ( const ImportedTextureSlot& slot : fill.Textures )
        {
            std::filesystem::path image = slot.Parts.front().Source;
            if ( slot.NeedsPacking() )
            {
                image = PackedTexturePath( slot );
                if ( const auto packed = PackTextureChannels( slot, image ); !packed )
                    return Common::MakeError<bool>( std::format( "material '{}' in '{}': {}", material.Name,
                                                                 sourcePath.generic_string(),
                                                                 packed.GetError() ) );
            }
            if ( static_cast<uint64_t>( ImportTexture( image.string() ) ) == 0 )
                return Common::MakeError<bool>( std::format( "material '{}' in '{}': texture '{}' for slot '{}' "
                                                             "was not imported (the texture importer logged why)",
                                                             material.Name, sourcePath.generic_string(),
                                                             image.generic_string(), slot.Slot ) );
            const std::filesystem::path asset = TextureImporter::AssetPathFor( image );
            const auto                  key   = Assets::ReadTextureAssetKey( asset );
            if ( !key.IsSuccess() || key.GetValue().Guid.IsNull() )
                return Common::MakeError<bool>( std::format( "material '{}': texture '{}' states no identity ({})",
                                                             material.Name, asset.generic_string(),
                                                             key.IsSuccess() ? "null GUID" : key.GetError() ) );
            data.Textures.push_back( { slot.Slot, Common::Content::AssetGuidToText( key.GetValue().Guid ),
                                       Common::AssetHandle::StableKeyForPath( asset ), slot.Sampler } );
        }
        data.Header = Common::Content::MakeTextHeader( Common::Content::ContentKind::Material, material.Guid,
                                                            Assets::MaterialTextSubsystems() );
        const auto text = Assets::WriteMaterialJson( data );
        if ( !text )
            return Common::MakeFormattedError<bool>( "material '{}' refused: {}", path.string(), text.GetError() );
        return WriteCookedBytes( text.GetValue(), path );
    }

    Common::UUID ImportManager::ImportTexture( const std::filesystem::path& path )
    {
        return m_TextureImporter->Import( path );
    }

    size_t ImportManager::ImportLooseTextures( const Assets::ItemProgress& progress )
    {
        // IMPORT, NOT COOK (AF3c). A loose image dropped under `LooseTextureRoots()` becomes its `.detex`
        // asset here; its platform data is derived on first use (Assets::LoadTexturePlatformData -> the
        // builder this editor registers) or by the packager, and lives in the DDC, never beside the asset.
        size_t                                   imported = 0;
        const std::vector<std::filesystem::path> sources  = LooseTextureSources();
        for ( std::size_t i = 0; i < sources.size(); ++i )
        {
            const std::filesystem::path& source = sources[i];
            Assets::ReportItem( progress, source.filename().string(), i, sources.size() );
            if ( source.extension() == Assets::kTextureAssetExtension )
                continue;
            if ( const auto asset = TextureImporter::ImportSourceAsset( source ); asset.IsSuccess() )
                ++imported;
            else
                LOG_ERROR( "[ImportManager] '{}' was not imported into a texture asset: {}", source.string(),
                           asset.GetError() );
        }
        return imported;
    }

    std::shared_ptr<Assets::TextureAsset> ImportManager::ImportTextureAsset( Assets::AssetManager&        mgr,
                                                                             const std::filesystem::path& source )
    {
        // Import + derive the texture (the importer logs a failure and returns the null handle), then create
        // the TextureAsset on its `.detex` -- the asset IS the file the runtime reads (header + DDC key).
        if ( static_cast<uint64_t>( m_TextureImporter->Import( source ) ) == 0 )
        {
            return nullptr;
        }

        const auto cookedMeta = TextureImporter::AssetPathFor( source );

        auto asset = mgr.CreateAsset<Assets::TextureAsset>( cookedMeta.string() );
        if ( !asset )
        {
            LOG_ERROR( "ImportTextureAsset: failed to create TextureAsset from {}", cookedMeta.string() );
            return nullptr;
        }
        return asset;
    }

} // namespace Desert::Editor