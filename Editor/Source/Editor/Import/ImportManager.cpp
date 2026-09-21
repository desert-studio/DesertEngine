#include "ImportManager.hpp"
#include <Common/Core/Serialization/GlmReflection.hpp>

#include "Assimp/AssimpImporter.hpp"
#include "Blend/BlendImporter.hpp"
#include "CookPaths.hpp"
#include "CookedJsonWrite.hpp"

#include <Engine/Assets/Serialization/MeshBinary.hpp>
#include "LODFold.hpp"

#include <Common/Core/Constants.hpp>

#include <Engine/Assets/TextureAsset.hpp>
#include <Engine/Geometry/MeshLOD.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Core/JobSystem.hpp>

#include <chrono>
#include <regex>

namespace Desert::Editor
{

    static std::filesystem::path BuildCookedPath( const std::filesystem::path& sourcePath,
                                                  const std::string&           extension )
    {
        // Path formula is shared (CookPaths::CookedMesh); this wrapper also ensures the dir exists for writing.
        const auto result = Editor::CookPaths::CookedMesh( sourcePath, extension );
        std::filesystem::create_directories( result.parent_path() );
        return result;
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
        // A cooked file counts as up-to-date if it exists and isn't older than its source.
        bool CookedFresh( const std::filesystem::path& source, const std::filesystem::path& cooked )
        {
            std::error_code ec;
            if ( !std::filesystem::exists( cooked, ec ) )
                return false;
            const auto cookedT = std::filesystem::last_write_time( cooked, ec );
            if ( ec )
                return false;
            const auto srcT = std::filesystem::last_write_time( source, ec );
            if ( ec )
                return false;
            return cookedT >= srcT;
        }
    } // namespace

    void ImportManager::Import( const std::filesystem::path& path, bool force )
    {
        auto ext = path.extension().string();
        std::transform( ext.begin(), ext.end(), ext.begin(), ::tolower );

        if ( !m_Importers.contains( ext ) )
            return;

        // Skip the expensive Assimp re-parse (+ its texture/material re-cook) when a cooked mesh output
        // already exists and is up-to-date. A source produces either a static or a skinned mesh, so accept
        // either. `force` (Rebuild Cooked Assets) bypasses this.
        if ( !force && ( CookedFresh( path, BuildCookedPath( path, ".stmesh" ) ) ||
                         CookedFresh( path, BuildCookedPath( path, ".skmesh" ) ) ) )
            return;

        auto result = m_Importers[ext]->Import( path, *this );
        // The cook's verdict stops HERE, at a log line naming the file and the reason. It has nowhere
        // further to go and that is deliberate rather than overlooked: this function is void because
        // both of its callers are fire-and-forget — the boot scan (ImportAllFromDirectory, on
        // JobSystem workers) and drag-and-drop. Widening it into a result would oblige every one of
        // those to grow an answer nobody is waiting for. What Д31-D asked for is that a cooked file
        // that was never written stops being INDISTINGUISHABLE from one that was; it now is.
        if ( const auto cooked = CreateAssetsFromImport( result, path ); !cooked )
            LOG_ERROR( "[Import] '{}' was parsed but its cooked output is incomplete: {}", path.string(),
                       cooked.GetError() );
    }

    void ImportManager::ImportAllFromDirectory( const std::filesystem::path& root, bool force )
    {
        namespace fs = std::filesystem;

        std::error_code ec;
        if ( !fs::exists( root, ec ) ) // tolerate a missing source dir (e.g. clean/from-scratch project)
            return;

        // Gather first, cook in PARALLEL after: each source file is an independent CPU+disk job (that is
        // exactly what AsyncMeshLoader relies on for single files). Each worker thread cooks on its OWN
        // ImportManager (Assimp importers are not reentrant); shared cooked-texture writes are serialized
        // inside TextureImporter.
        std::vector<fs::path> files;
        for ( const auto& entry : fs::recursive_directory_iterator( root, ec ) )
        {
            if ( !entry.is_regular_file() )
                continue;

            std::string ext = entry.path().extension().string();
            std::transform( ext.begin(), ext.end(), ext.begin(), ::tolower );

            // .blend is converted via a (potentially multi-minute) headless Blender run — far too heavy to do
            // for every file during a blocking bulk scan at boot. It's imported ON DEMAND instead (drag-drop
            // goes through AsyncMeshLoader on a worker thread), so the editor never freezes on it.
            if ( ext == ".blend" )
                continue;

            if ( m_Importers.contains( ext ) )
                files.push_back( entry.path() );
        }

        if ( files.empty() )
            return;

        if ( files.size() == 1 )
        {
            Import( files.front(), force );
            return;
        }

        const auto started = std::chrono::steady_clock::now();
        Common::JobSystem::Get().ParallelFor( files.size(),
                                              [&]( size_t i )
                                              {
                                                  // One cooker per worker thread, reused across files.
                                                  thread_local ImportManager s_ThreadImporter;
                                                  s_ThreadImporter.Import( files[i], force );
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
    Common::BoolResultStr ImportManager::CreateAssetsFromImport( const ImportResult&          result,
                                                                 const std::filesystem::path& sourcePath )
    {
        std::string firstFailure;
        const auto  record = [&firstFailure]( const Common::BoolResultStr& outcome )
        {
            if ( !outcome && firstFailure.empty() )
                firstFailure = outcome.GetError();
        };

        if ( result.Mesh )
            record( SerializeMeshAsset( result.Mesh.value(), sourcePath ) );

        if ( result.Skeleton )
            record( SerializeSkeletonAsset( result.Skeleton.value(), sourcePath ) );

        for ( const auto& anim : result.Animations )
            record( SerializeAnimationAsset( anim, sourcePath ) );

        for ( const auto& material : result.Materials )
            record( SerializeMaterialAsset( material, sourcePath ) );

        if ( !firstFailure.empty() )
            return Common::MakeError<bool>( firstFailure );
        return BOOLSUCCESS;
    }

    namespace
    {
        // Bakes each static submesh's LOD triangle sets at cook time (meshopt) into SubmeshData.LODs, so the
        // load path can skip the simplification pass. Skinned meshes are left un-LODed (as before).
        void BakeStaticMeshLODs( Desert::Assets::Serialization::MeshAssetData& data )
        {
            if ( data.IsSkinned )
                return;
            for ( auto& sm : data.Submeshes )
            {
                if ( !sm.LODs.empty() )
                    continue; // author LODs already folded in (FoldExternalLODMeshes) -> don't regenerate

                const uint32_t triCount = sm.IndexCount / 3;
                if ( triCount < 8 || sm.VertexCount == 0 ||
                     sm.VertexOffset + sm.VertexCount > data.StaticVertices.size() )
                    continue;

                std::vector<float> pos;
                pos.reserve( static_cast<size_t>( sm.VertexCount ) * 3 );
                for ( uint32_t v = 0; v < sm.VertexCount; ++v )
                {
                    const auto& p = data.StaticVertices[sm.VertexOffset + v].Position;
                    pos.push_back( p.x );
                    pos.push_back( p.y );
                    pos.push_back( p.z );
                }

                std::vector<Desert::Index> localTris;
                localTris.reserve( triCount );
                const uint32_t triStart = sm.IndexOffset / 3;
                if ( triStart + triCount > data.Indices.size() )
                    continue;
                for ( uint32_t t = 0; t < triCount; ++t )
                {
                    const auto& idx = data.Indices[triStart + t];
                    localTris.push_back( { idx.V1, idx.V2, idx.V3 } );
                }

                const auto levels = Desert::Geometry::SimplifyLODLevels( pos.data(), sm.VertexCount, localTris );
                sm.LODs.clear();
                sm.LODs.reserve( levels.size() );
                for ( const auto& lvl : levels )
                {
                    std::vector<Desert::Assets::Serialization::IndexData> tris;
                    tris.reserve( lvl.size() );
                    for ( const auto& tri : lvl )
                        tris.push_back( { tri.V1, tri.V2, tri.V3 } );
                    sm.LODs.push_back( std::move( tris ) );
                }
            }
        }
    } // namespace

    Common::BoolResultStr
    ImportManager::SerializeMeshAsset( const Desert::Assets::Serialization::MeshAssetData& dataIn,
                                       const std::filesystem::path&                        sourcePath )
    {
        // Mutable copy so we can bake the LOD chain into it before writing.
        Desert::Assets::Serialization::MeshAssetData data = dataIn;
        FoldExternalLODMeshes( data ); // author "<mesh>_LOD<n>" siblings -> SubmeshData.LODs
        BakeStaticMeshLODs( data );    // generate LODs for submeshes that still have none

        std::filesystem::path cookedPath;
        if ( data.IsSkinned )
        {
            cookedPath = BuildCookedPath( sourcePath, ".skmesh" );
        }
        else
        {
            cookedPath = BuildCookedPath( sourcePath, ".stmesh" );
        }

        // THE ONE PLACE A COOKED MESH IS WRITTEN, and since B11 it writes the binary container rather
        // than JSON. The sibling cooked kinds beside this one (.skeleton, .anim, .demat, .tex metadata)
        // are unchanged: they are kilobytes of structure, not megabytes of floats, and the argument
        // that moved this one does not reach them.
        return WriteCookedBytes( Desert::Assets::Serialization::EncodeMeshBinary( data ), cookedPath );
    }

    Common::BoolResultStr
    ImportManager::SerializeSkeletonAsset( const Desert::Assets::Serialization::SkeletonAssetData& data,
                                           const std::filesystem::path&                            sourcePath )
    {
        auto cookedPath = BuildCookedPath( sourcePath, ".skeleton" );
        return WriteCookedJson( data, cookedPath );
    }

    Common::BoolResultStr
    ImportManager::SerializeAnimationAsset( const Desert::Assets::Serialization::AnimationAssetData& data,
                                            const std::filesystem::path&                             sourcePath )
    {
        auto cookedPath = BuildCookedPath( sourcePath, "_" + data.Name + ".anim" );
        return WriteCookedJson( data, cookedPath );
    }

    Common::BoolResultStr ImportManager::SerializeMaterialAsset( const ImportedMaterial&      material,
                                                                 const std::filesystem::path& sourcePath )
    {
        // Imported materials are EDITABLE CONTENT, not cooked intermediates -> write them into the content
        // tree at Resources/Assets/Materials/<meshRelativeId>/<materialName>.demat (browsable + editable in
        // the asset browser, reusable), like UE.
        // Meaningful name, NO handle in it (stable identity lives in the file: PBRSurfaceParams::MaterialId).
        // Unified .demat schema (legacy ".mat" cooker output is gone; SurfaceMaterialAsset::Load still READS old).
        //
        // The per-mesh subfolder is CookPaths::MaterialFolder and not `MATERIAL_PATH / stem` spelled here.
        // The stem alone put two same-named meshes from different folders in one folder, where the
        // "only write if MISSING" rule below turns a collision into a silent adoption: the second mesh's
        // materials are never written and it inherits the first mesh's instead.
        static const std::regex illegal( R"([<>:"/\\|?*\s])" );
        const std::string       safeName = std::regex_replace( material.Name, illegal, "_" );
        const std::filesystem::path path =
             CookPaths::MaterialFolder( sourcePath ) /
             ( safeName + std::string( Common::Constants::Extensions::MATERIAL_EXTENSION ) );

        // Only write if MISSING: re-importing a mesh must NOT clobber the user's edits to its material (UE
        // behaviour — re-import updates geometry, keeps the material asset). Delete the .demat to regenerate.
        std::error_code ec;
        if ( std::filesystem::exists( path, ec ) )
            return BOOLSUCCESS; // deliberately kept, not a failure to write
        // Typed extraction -> unified canon (the only on-disk material format).
        return WriteCookedJson( material.Data.ToMaterialData(), path );
    }

    Common::UUID ImportManager::ImportTexture( const std::filesystem::path& path )
    {
        return m_TextureImporter->Import( path );
    }

    Assets::AssetHandle ImportManager::ImportAndRegisterTexture( Assets::AssetManager&         mgr,
                                                                 const std::filesystem::path& source )
    {
        // Cook the source -> Cooked/Textures/<name>.tex (metadata names the source by its root-tagged key).
        // A failed cook (the importer logged why) returns the null handle and writes NO .tex, so stop here:
        // CreateAsset on the missing cooked file would only fail later, inside TextureAsset::Load, with a
        // read error about a .tex this function already knows was never written.
        if ( static_cast<uint64_t>( m_TextureImporter->Import( source ) ) == 0 )
        {
            return Common::UUID::Null();
        }

        const auto cookedMeta = TextureImporter::CookedMetaPath( source );

        // Create + load the TextureAsset from the cooked .tex (Load reads the handle + source path, and
        // syncs the metadata handle), then register it so TextureService can resolve it at draw time.
        auto asset = mgr.CreateAsset<Assets::TextureAsset>( Assets::AssetPriority::Low, cookedMeta.string() );
        if ( !asset )
        {
            LOG_ERROR( "ImportAndRegisterTexture: failed to create TextureAsset from {}",
                       cookedMeta.string() );
            return Common::UUID::Null();
        }

        Runtime::ResourceRegistry::GetTextureService()->Register( asset );
        return asset->GetMetadata().Handle;
    }

} // namespace Desert::Editor