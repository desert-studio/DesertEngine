#include <Engine/Assets/Serialization/ImportRecord.hpp>
#include <Common/Content/ContentScan.hpp>
#include "MeshDnD.hpp"
#include "ImportManager.hpp"
#include "ImportedMeshAsset.hpp"
#include "CookPaths.hpp"

#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/Mesh/MeshAsset.hpp>
#include <Engine/Assets/Mesh/StaticMeshAsset.hpp>
#include <Engine/Assets/Mesh/SkinnedMeshAsset.hpp>
#include <Engine/Assets/Mesh/SurfaceMaterialAsset.hpp>
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

        // Same, for a rigged source that imports to a skinned mesh beside it (Resources/Assets/Meshes/foo.skmesh).
        std::filesystem::path CookedSkinnedMeshPath( const std::string& sourcePath )
        {
            return CookPaths::SkinnedAsset( sourcePath, ".skmesh" );
        }

        // A FRESH IMPORT IS IN NOBODY'S REGISTRY YET, and MeshService names a skinned mesh's rig by the
        // registry's Rig tag (AL1-5). So the new .skmesh and the rig and clips written beside it
        // (CookPaths::SkinnedAsset: <stem>.skeleton, <stem>_<clip>.anim) are noted now — their headers are
        // read by the registry, no asset is loaded. Only this import's siblings: the folder is authored
        // content, and a stranger's file in it is not this import's business.
        void NoteFreshSkinnedCook( const std::filesystem::path& skinned )
        {
            namespace fs = std::filesystem;
            Assets::ContentRegistry::Update( skinned );
            const std::string stem = skinned.stem().string();
            std::error_code   ec;
            for ( const auto& f : fs::directory_iterator( skinned.parent_path(), ec ) )
            {
                const fs::path&   file = f.path();
                const std::string name = file.stem().string();
                const bool        rig  = file.extension() == ".skeleton" && name == stem;
                const bool        clip = file.extension() == ".anim" && name.starts_with( stem + "_" );
                if ( f.is_regular_file( ec ) && ( rig || clip ) )
                    Assets::ContentRegistry::Update( file );
            }
        }

        // Create + register a freshly-imported mesh's materials so their stable external id
        // (PBRSurfaceParams::MaterialId, baked into each submesh) resolves in MaterialService THIS session.
        // Without this the materials would only register when something next names them and a
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
                    asset = mgr.CreateAsset<Assets::SurfaceMaterialAsset>( matPath,
                                                                           /*loadAfterCreate=*/false );
                if ( !asset )
                    continue;
                // Registered UNREAD (AL1-5b): the shell's key is its header GUID, and the first Get starts
                // the read on a worker. The `!Get( h )` guard that stood here BUILT the runtime material,
                // textures and all, to decide whether the material needed registering.
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
        // both of. It never showed because a boot scanner (since deleted) registered every cooked mesh before the
        // editor could accept a drop — a safety net, not a guarantee.
        if ( auto existing = mgr.FindByPath<Assets::MeshAsset>( cookedStr ) )
        {
            Runtime::EnsureMeshRegistered( existing, mgr );
            return existing->GetMetadata().Handle;
        }

        // Not cooked yet -> cook the source (Assimp parse -> Cooked/Meshes/*.stmesh). AF4h: an import
        // never writes `cookedStr` to disk any more (the source envelope lives in the DDC), so
        // `exists(cookedStr)` alone answered "not cooked" for every freshly imported mesh forever and the
        // drop silently placed nothing. StaticMeshCookAvailable also accepts a fresh DDC envelope for
        // `sourcePath`.
        if ( !StaticMeshCookAvailable( cookedStr, sourcePath ) )
            Importer().Import( sourcePath );

        if ( !StaticMeshCookAvailable( cookedStr, sourcePath ) )
            return Common::UUID::Null(); // cook failed / produced a skinned mesh (.skmesh) instead

        // THE SOURCE'S IDENTITY (FIX8): a source cooked before its record existed (the DDC already held its
        // envelope, so no import ran) is imported now - the import writes the record with the mesh's box
        // (DIMP 2), which only the imported mesh knows. The asset below reads its GUID from it.
        if ( std::error_code ec;
             !std::filesystem::is_regular_file( Common::Content::ImportRecordPathFor( sourcePath ), ec ) )
            Importer().Import( sourcePath );
        if ( const auto identity = Assets::Serialization::ReadImportRecordGuid( sourcePath ); !identity )
        {
            LOG_ERROR( "[MeshDnD] {}", identity.GetError() );
            return Common::UUID::Null();
        }

        // Create + register + load the cooked static mesh, return its handle.
        auto created = mgr.CreateAsset<Assets::StaticMeshAsset>( cookedStr,
                                                                 /*loadAfterCreate=*/false );
        if ( !created )
            return Common::UUID::Null();

        // REGISTER AND BUILD NOW, because a drop must refuse a cook that produced nothing rather than place
        // an entity that draws air. (The `created->Load()` that used to follow the registration is gone for
        // the reason MeshService.hpp gives: registration parses before it builds, so a load after it could
        // only ever run once an empty mesh had been cached.)
        Runtime::EnsureMeshRegistered( created, mgr ); // read by the loader, drawn when it lands

        // UE-style: a just-imported mesh's materials are immediately available (no restart/Save). Its
        // textures need no step here: TextureImporter noted each `.detex` in the content registry, and
        // TextureService discovers and reads them on a worker when a material first binds them (AL1-5b).
        RegisterCookedMaterials( mgr, sourcePath );
        return created->GetMetadata().Handle;
    }

    namespace
    {
        // A skinned mesh is REGISTERED here, not read: MeshService requests it and its rig (named by the
        // registry's Rig tag) from the loader, and the entity draws from the frame after they land (AL1-5).
        // The cooked materials beside it are registered only after a fresh import, which is the one case that
        // has just written them; their textures are discovered from the content registry on first bind.
        Assets::AssetHandle FinalizeSkinned( Assets::AssetManager&                     mgr,
                                             const std::shared_ptr<Assets::MeshAsset>& asset )
        {
            Runtime::EnsureMeshRegistered( asset, mgr );
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
            return { FinalizeSkinned( mgr, existing ), true };
        if ( auto existing = mgr.FindByPath<Assets::MeshAsset>( staticStr ) )
        {
            // The same registration the create path below performs. The skinned twin above already did it
            // (through FinalizeSkinned); this one did not, and the asymmetry between two adjacent lines is
            // exactly the shape a single registrar removes.
            Runtime::EnsureMeshRegistered( existing, mgr );
            return { existing->GetMetadata().Handle, false };
        }

        // Cook on demand if neither cooked form exists yet (the cook decides static vs skinned by the rig).
        // StaticMeshCookAvailable, not exists(): since AF4h an imported static mesh's envelope lives in the DDC
        // and nothing is written at staticStr, so exists() read every fresh cook as "cook failed".
        if ( !StaticMeshCookAvailable( staticStr, sourcePath ) && !std::filesystem::exists( skinnedStr ) )
            (void)Importer().Import( sourcePath );

        const bool isSkinned = std::filesystem::exists( skinnedStr );
        const bool isStatic  = StaticMeshCookAvailable( staticStr, sourcePath );
        if ( !isSkinned && !isStatic )
            return { Common::UUID::Null(), false }; // cook failed

        if ( isSkinned )
        {
            // Created as an UNPARSED shell: MeshService reads it and its rig through the loader.
            NoteFreshSkinnedCook( skinnedStr );
            auto created = mgr.CreateAsset<Assets::SkinnedMeshAsset>( skinnedStr,
                                                                      /*loadAfterCreate=*/false );
            if ( !created )
                return { Common::UUID::Null(), false };
            RegisterCookedMaterials( mgr, sourcePath );
            return { FinalizeSkinned( mgr, created ), true };
        }

        auto created = mgr.CreateAsset<Assets::StaticMeshAsset>( staticStr,
                                                                 /*loadAfterCreate=*/false );
        if ( !created )
            return { Common::UUID::Null(), false };

        Runtime::EnsureMeshRegistered( created, mgr ); // read by the loader, drawn when it lands
        RegisterCookedMaterials( mgr, sourcePath );
        return { created->GetMetadata().Handle, false };
    }

} // namespace Desert::Editor::MeshDnD
