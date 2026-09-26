#include <Common/Content/ContentScan.hpp>
#include "MeshDnD.hpp"
#include "ImportManager.hpp"
#include "CookPaths.hpp"

#include <Engine/Assets/Mesh/MeshAsset.hpp>
#include <Engine/Assets/Mesh/StaticMeshAsset.hpp>
#include <Engine/Assets/Mesh/SkinnedMeshAsset.hpp>
#include <Engine/Assets/Mesh/SurfaceMaterialAsset.hpp>
#include <Engine/Assets/TextureAsset.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/AssetServiceRegistration.hpp>
#include <Common/Core/Constants.hpp>

#include <filesystem>

namespace Desert::Editor::MeshDnD
{
    namespace
    {
        // One shared importer for drag-drop cooks (the cooked-file freshness check dedups re-cooks).
        ImportManager& Importer()
        {
            static ImportManager s_Importer;
            return s_Importer;
        }

        // Source (Resources/Assets/Meshes/foo.obj) -> its mesh asset beside it
        // (Resources/Assets/Meshes/foo.stmesh).
        std::filesystem::path CookedStaticMeshPath( const std::string& sourcePath )
        {
            return CookPaths::MeshAsset( sourcePath );
        }

        // Same, for a rigged source that cooks to a skinned mesh (Cooked/Meshes/foo.skmesh).
        std::filesystem::path CookedSkinnedMeshPath( const std::string& sourcePath )
        {
            return CookPaths::CookedSkinned( sourcePath, ".skmesh" );
        }

        // Register every texture asset (`.detex` of kind Texture under the assets root) into the AssetManager +
        // TextureService so a just-imported material's texture handles RESOLVE this session. MaterialFactory binds
        // textures EAGERLY at material-register time (GetTextureService()->Get(handle)), so textures MUST be
        // registered BEFORE the materials — otherwise the bind silently no-ops and the slot shows "missing" until
        // the next launch (when AssetPreloader scans them). Idempotent (skips already-registered). Mirrors the
        // preloader's texture loop. The mesh's cook already imported these assets via ExtractMaterials.
        void RegisterCookedTextures( Assets::AssetManager& mgr )
        {
            namespace fs = std::filesystem;
            std::error_code ec;
            const fs::path  root = Common::Constants::Path::ASSETS_PATH;
            if ( !fs::exists( root, ec ) )
                return;

            for ( const auto& f : fs::recursive_directory_iterator( root, ec ) )
            {
                if ( !f.is_regular_file( ec ) ||
                     Common::Content::KindOfContentFile( f.path() ) != Common::Content::ContentKind::Texture )
                    continue;
                const std::string p = f.path().generic_string();
                auto asset = mgr.FindByPath<Assets::TextureAsset>( p );
                if ( !asset )
                    asset = mgr.CreateAsset<Assets::TextureAsset>( Assets::AssetPriority::Low, p );
                if ( !asset )
                    continue;
                if ( !asset->IsReadyForUse() )
                    asset->Load(); // syncs metadata handle to the asset header's handle (material refs key by it)
                // Was `if ( !GetTextureService()->Get( h ) ) Register( a )`: the guard UPLOADED the texture
                // it was asking about, and the registration behind it uploaded it again. One shell write.
                Runtime::EnsureTextureRegistered( mgr, static_cast<uint64_t>( asset->GetMetadata().Handle ) );
            }
        }

        // The cook's own suffix for a rig, spelled here because Common::Constants::Extensions has no entry
        // for it and AssetPreloader's scan list carries the same literal.
        constexpr const char* kSkeletonExtension = ".skeleton";

        // Register every cooked skeleton (Cooked/Meshes/*.skeleton) into the AssetManager so a
        // just-imported RIGGED mesh finds its rig THIS session.
        //
        // WHY THIS HAS TO EXIST AT ALL: AssetPreloader.cpp was, until now, the only place in the engine that
        // ever constructed a SkeletonAsset. A drop that cooks a new .skmesh also cooks its .skeleton, and
        // nothing put that file in the manager — so the mesh's signature matched nothing, MeshFactory
        // refused to build it, and the character only appeared after a relaunch. Same shape as the textures
        // and materials above: the cook wrote a file the session cannot see.
        void RegisterCookedSkeletons( Assets::AssetManager& mgr )
        {
            namespace fs = std::filesystem;
            std::error_code ec;
            const fs::path  root = Common::Constants::Path::MESH_PATH_COOKED;
            if ( !fs::exists( root, ec ) )
                return;

            for ( const auto& f : fs::recursive_directory_iterator( root, ec ) )
            {
                if ( !f.is_regular_file( ec ) || f.path().extension() != kSkeletonExtension )
                    continue;

                const std::string p = f.path().generic_string();
                if ( mgr.FindByPath<Assets::SkeletonAsset>( p ) )
                    continue; // idempotent: a drop re-scans the whole directory every time

                // Eager, like the preloader's scan: a skeleton is small, and its signature is what every
                // skinned mesh in the project is matched against — an unloaded one reports 0 and matches
                // nothing.
                if ( !mgr.CreateAsset<Assets::SkeletonAsset>( Assets::AssetPriority::Low, p ) )
                    LOG_ERROR( "Cooked skeleton '{}' could not be registered.", p );
            }
        }

        // Create + register a freshly-imported mesh's materials so their stable external id
        // (PBRSurfaceParams::MaterialId, baked into each submesh) resolves in MaterialService THIS session.
        // Without this the materials would only register on the NEXT launch (AssetPreloader scan) and a
        // just-imported mesh shows "Unassigned material slot". Import writes them as editable content at
        // CookPaths::MaterialFolder(source) (see ImportManager::SerializeMaterialAsset).
        // Idempotent (skips already-registered).
        //
        // Takes the SOURCE path and asks CookPaths, rather than rebuilding the folder from the cooked
        // path's stem. The old spelling here, `MATERIAL_PATH / cookedMeshPath.stem()`, was a second copy
        // of the writer's formula that happened to agree with it only because both discarded the
        // directory — so making the writer directory-aware without this would have left the reader looking
        // in a folder nothing is written to, and every freshly imported mesh unassigned until relaunch.
        void RegisterCookedMaterials( Assets::AssetManager& mgr, const std::filesystem::path& sourcePath )
        {
            namespace fs = std::filesystem;
            std::error_code ec;
            const fs::path  dir = CookPaths::MaterialFolder( sourcePath );
            if ( !fs::exists( dir, ec ) )
                return;

            for ( const auto& f : fs::directory_iterator( dir, ec ) )
            {
                if ( f.path().extension() != Common::Constants::Extensions::MATERIAL_EXTENSION )
                    continue;

                const std::string matPath = f.path().generic_string();
                auto asset = mgr.FindByPath<Assets::SurfaceMaterialAsset>( matPath );
                if ( !asset )
                    asset = mgr.CreateAsset<Assets::SurfaceMaterialAsset>( Assets::AssetPriority::High, matPath );
                if ( !asset )
                    continue;
                // The load is inside EnsureMaterialRegistered now, for the reason the comment that stood
                // here gave: Load reads MaterialId, which is the key the service maps the material under.
                // The `!Get( h )` guard is gone with it — it BUILT the runtime material, textures and all,
                // to decide whether the material needed registering.
                Runtime::EnsureMaterialRegistered( asset );
            }
        }
    } // namespace

    Assets::AssetHandle ResolveOrImport( Assets::AssetManager& mgr, const std::string& sourcePath )
    {
        const std::string cookedStr = CookedStaticMeshPath( sourcePath ).generic_string();

        // ALREADY COOKED? Reuse it — and register it, which is what the question mark in the comment that
        // stood here ("Already cooked + registered?") was standing in for. Finding a record proves the
        // registry has it; it proves nothing about the mesh SERVICE, and the two are what a drop needs
        // both of. It never showed because AssetPreloader had registered every cooked mesh before the
        // editor could accept a drop — a safety net, not a guarantee.
        if ( auto existing = mgr.FindByPath<Assets::MeshAsset>( cookedStr ) )
        {
            Runtime::EnsureMeshRegistered( existing, mgr );
            return existing->GetMetadata().Handle;
        }

        // Not cooked yet -> cook the source (Assimp parse -> Cooked/Meshes/*.stmesh).
        if ( !std::filesystem::exists( cookedStr ) )
            Importer().Import( sourcePath );

        if ( !std::filesystem::exists( cookedStr ) )
            return Common::UUID::Null(); // cook failed / produced a skinned mesh (.skmesh) instead

        // Create + register + load the cooked static mesh, return its handle.
        auto created = mgr.CreateAsset<Assets::StaticMeshAsset>( Assets::AssetPriority::High, cookedStr );
        if ( !created )
            return Common::UUID::Null();

        // REGISTER AND BUILD NOW, because a drop must refuse a cook that produced nothing rather than place
        // an entity that draws air. (The `created->Load()` that used to follow the registration is gone for
        // the reason MeshService.hpp gives: registration parses before it builds, so a load after it could
        // only ever run once an empty mesh had been cached.)
        if ( const auto readiness = Runtime::EnsureMeshDrawable( created, mgr );
             readiness == Runtime::MeshReadiness::NotRegistered || readiness == Runtime::MeshReadiness::NotBuilt )
        {
            LOG_ERROR( "[Import] {}", Runtime::ExplainMeshReadiness( readiness, cookedStr ) );
            return Common::UUID::Null();
        }

        // UE-style: a just-imported mesh's materials + textures are immediately available (no restart/Save).
        // Order matters: textures FIRST (materials bind them eagerly at register time), then materials.
        RegisterCookedTextures( mgr );
        RegisterCookedMaterials( mgr, sourcePath );
        return created->GetMetadata().Handle;
    }

    namespace
    {
        // Finalize a SKINNED mesh asset so its GPU build works.
        //
        // The rig goes in FIRST. A shell's skeleton is matched by a signature that only exists once the
        // .skmesh is parsed, so the load below is the moment the lookup becomes possible — and the lookup
        // can only succeed against skeletons the manager already holds. On a fresh import that file was
        // written seconds ago by the cook and is in nobody's registry.
        //
        // This used to spell the load as `Load()` followed by `ResolveDependencies()`. Those two statements
        // are now one call, because the second is the one that gets forgotten (AssetBase::EnsureLoaded), and
        // MeshService's own lazy path forgot it for every mesh that was not dropped by hand.
        Assets::AssetHandle FinalizeSkinned( Assets::AssetManager&                     mgr,
                                             const std::shared_ptr<Assets::MeshAsset>& asset,
                                             const std::string&                        sourcePath )
        {
            RegisterCookedSkeletons( mgr );

            if ( const auto loaded = asset->EnsureLoaded( mgr ); !loaded )
            {
                LOG_ERROR( "Skinned mesh '{}' could not be finalized: {}", asset->GetMetadata().Filepath.string(),
                           loaded.GetError() );
                return Common::UUID::Null();
            }

            // Rebuild with the resolved skeleton. Reported rather than dropped: the caller receives a handle
            // either way, and a refusal here (the rig still missing, the .skmesh unparsable) is the one
            // moment the reason is knowable.
            //
            // `NoSubmeshes` IS NOT A REFUSAL HERE, and that is the whole reason the four states are an enum
            // rather than a bool: a skinned mesh's STATIC buffer is empty by design, so the state that
            // makes a thumbnail unphotographable is the ordinary state of the thing being imported.
            const auto readiness = Runtime::EnsureMeshDrawable( asset, mgr );
            if ( readiness == Runtime::MeshReadiness::NotRegistered ||
                 readiness == Runtime::MeshReadiness::NotBuilt )
            {
                LOG_ERROR( "Skinned mesh could not be finalized: {}",
                           Runtime::ExplainMeshReadiness( readiness, asset->GetMetadata().Filepath.string() ) );
                return Common::UUID::Null();
            }
            RegisterCookedTextures( mgr );
            RegisterCookedMaterials( mgr, sourcePath );
            return asset->GetMetadata().Handle;
        }
    } // namespace

    ResolvedMesh ResolveOrImportMesh( Assets::AssetManager& mgr, const std::string& sourcePath )
    {
        const std::string staticStr  = CookedStaticMeshPath( sourcePath ).generic_string();
        const std::string skinnedStr = CookedSkinnedMeshPath( sourcePath ).generic_string();

        // Already cooked + registered? Reuse it — but a skinned shell from the preloader is lazy + has an
        // UNRESOLVED skeleton, so finalize it (load + resolve + rebuild) instead of returning it raw.
        if ( auto existing = mgr.FindByPath<Assets::MeshAsset>( skinnedStr ) )
            return { FinalizeSkinned( mgr, existing, sourcePath ), true };
        if ( auto existing = mgr.FindByPath<Assets::MeshAsset>( staticStr ) )
        {
            // The same registration the create path below performs. The skinned twin above already did it
            // (through FinalizeSkinned); this one did not, and the asymmetry between two adjacent lines is
            // exactly the shape a single registrar removes.
            Runtime::EnsureMeshRegistered( existing, mgr );
            return { existing->GetMetadata().Handle, false };
        }

        // Cook on demand if neither cooked form exists yet (the cook decides static vs skinned by the rig).
        if ( !std::filesystem::exists( staticStr ) && !std::filesystem::exists( skinnedStr ) )
            Importer().Import( sourcePath );

        const bool isSkinned = std::filesystem::exists( skinnedStr );
        const bool isStatic  = std::filesystem::exists( staticStr );
        if ( !isSkinned && !isStatic )
            return { Common::UUID::Null(), false }; // cook failed

        if ( isSkinned )
        {
            // Created as an UNPARSED shell on purpose: loading here would resolve the skeleton against a
            // manager that does not hold the rig yet — the cook wrote the .skeleton one line ago — and the
            // resolve would then never be repeated. FinalizeSkinned registers the rig and loads, in that
            // order, and is the only place either happens.
            auto created = mgr.CreateAsset<Assets::SkinnedMeshAsset>( Assets::AssetPriority::High, skinnedStr,
                                                                      /*loadAfterCreate=*/false );
            if ( !created )
                return { Common::UUID::Null(), false };
            return { FinalizeSkinned( mgr, created, sourcePath ), true };
        }

        auto created = mgr.CreateAsset<Assets::StaticMeshAsset>( Assets::AssetPriority::High, staticStr );
        if ( !created )
            return { Common::UUID::Null(), false };

        if ( const auto readiness = Runtime::EnsureMeshDrawable( created, mgr );
             readiness == Runtime::MeshReadiness::NotRegistered || readiness == Runtime::MeshReadiness::NotBuilt )
        {
            LOG_ERROR( "[Import] {}", Runtime::ExplainMeshReadiness( readiness, staticStr ) );
            return { Common::UUID::Null(), false };
        }
        RegisterCookedTextures( mgr );
        RegisterCookedMaterials( mgr, sourcePath );
        return { created->GetMetadata().Handle, false };
    }

} // namespace Desert::Editor::MeshDnD
