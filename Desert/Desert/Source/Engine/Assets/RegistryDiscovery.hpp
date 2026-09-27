#pragma once

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/AsyncAssetLoader.hpp>
#include <Engine/Assets/ContentRegistry.hpp>

#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/ResultStr.hpp>

#include <filesystem>
#include <memory>
#include <string>

namespace Desert::Assets
{
    // AN ON-DEMAND KIND'S SHELL, CREATED FROM ITS REGISTRY ROW AT THE MOMENT SOMETHING NAMES IT.
    //
    // This replaces the boot-time loop that created a shell for every `.dcnv`/`.dcmv`/`.dclayout` whether
    // or not anything referenced it (AL1-2; UE's soft reference resolved through the asset registry). The
    // shell is created UNREAD — `loadAfterCreate = false` — so asking costs a header read, and the bytes
    // still arrive through the service's AsyncAssetLoader request, which is the one loader there is.
    //
    // Every failure is an error carrying what the caller needs to fix the content: the handle when there is
    // no row, the path and the GUID when the row names a file that is not on disk. A missing file used to
    // produce a sky with no clouds and one generic line; that is the silent fallback this refuses.
    // The path is printed forward-slashed: the row's spelling is whatever the directory scan joined, which
    // on Windows mixes both separators, and a message that is searched for (by a person or a test) needs
    // one spelling on every platform.
    template <typename AssetType>
    Common::ResultStr<Asset<AssetType>> CreateFromRegistryRow( const std::weak_ptr<AssetManager>& assets,
                                                               const AssetHandle&                 handle,
                                                               const Common::Content::ContentKind kind )
    {
        const auto manager = assets.lock();
        if ( !manager )
            return Common::MakeFormattedError<Asset<AssetType>>(
                 "{} {} was asked for, but no asset manager is bound to create it in",
                 Common::Content::KindName( kind ), static_cast<uint64_t>( handle ) );

        if ( auto existing = manager->ProbeByHandle<AssetType>( handle ) )
            return Common::MakeSuccess( std::move( existing ) );

        const auto row = ContentRegistry::RowOf( kind, static_cast<uint64_t>( handle ) );
        if ( !row )
            return Common::MakeFormattedError<Asset<AssetType>>(
                 "{} {} is referenced, but the content registry has no row of that kind under that number; "
                 "the reference names a file that was never scanned or cooked",
                 Common::Content::KindName( kind ), static_cast<uint64_t>( handle ) );

        const std::string guid =
             row->Guid ? Common::Content::AssetGuidToText( *row->Guid ) : std::string( "<none>" );
        std::error_code ec;
        if ( !std::filesystem::exists( row->Path, ec ) )
            return Common::MakeFormattedError<Asset<AssetType>>(
                 "{} '{}' (GUID {}, handle {}) is in the content registry but the file is not on disk",
                 Common::Content::KindName( kind ), row->Path.generic_string(), guid,
                 static_cast<uint64_t>( handle ) );

        auto created = manager->CreateAsset<AssetType>( AssetPriority::Medium, row->Path,
                                                        /*loadAfterCreate=*/false );
        if ( !created )
            return Common::MakeFormattedError<Asset<AssetType>>( "{} '{}' (GUID {}) could not be created",
                                                                 Common::Content::KindName( kind ),
                                                                 row->Path.generic_string(), guid );

        // The service keys its entry by the number it was asked for; a shell that adopted a DIFFERENT number
        // (a header whose GUID disagrees with the registry's) would be announced under it and never answer.
        if ( created->GetMetadata().Handle != handle )
            return Common::MakeFormattedError<Asset<AssetType>>(
                 "{} '{}' was asked for as {} but its header names {} (registry GUID {}); re-cook the registry",
                 Common::Content::KindName( kind ), row->Path.generic_string(), static_cast<uint64_t>( handle ),
                 static_cast<uint64_t>( created->GetMetadata().Handle ), guid );

        return Common::MakeSuccess( std::move( created ) );
    }

    // The registry row of `kind` whose header states @p guid, created as an (unread) asset when the
    // manager does not hold it yet. What a reference by GUID resolves through: the row, not the set of
    // assets somebody happened to create first.
    template <typename AssetType>
    Asset<AssetType> CreateFromRegistryGuid( AssetManager& manager, const Common::Content::AssetGuid& guid,
                                             const Common::Content::ContentKind kind )
    {
        for ( const auto& row : ContentRegistry::Rows( kind ) )
        {
            if ( row.Guid && *row.Guid == guid )
                return manager.CreateAsset<AssetType>( AssetPriority::Medium, row.Path,
                                                       /*loadAfterCreate=*/false );
        }
        return nullptr;
    }

    // Plan 2.4(b)/(c): a caller that must have @p asset read before it returns (opening a scene, an
    // editor the user is waiting on). It still goes through the loader - `Request` + `FlushOne` - so
    // SyncLoadLedger sees the read, and a read a worker already started is waited for, not repeated.
    // Dependencies are resolved against @p manager once the body is in.
    template <typename AssetType>
    Common::BoolResultStr LoadThroughLoader( AssetManager& manager, const Asset<AssetType>& asset )
    {
        if ( !asset )
            return Common::MakeError<bool>( "no asset to load" );
        if ( asset->IsReadyForUse() )
            return BOOLSUCCESS;

        std::string failure = "the loader delivered nothing";
        bool        loaded  = false;
        LoadRequest request = AsyncAssetLoader::Get().Request(
             asset,
             [&]( const Asset<AssetBase>&, const LoadOutcome outcome, const std::string& error )
             {
                 loaded  = outcome == LoadOutcome::Loaded;
                 failure = error;
             },
             [&] { failure = "the request was cancelled"; } );
        AsyncAssetLoader::Get().AwaitOne( asset->GetMetadata().Handle );
        if ( !loaded || !asset->IsReadyForUse() )
            return Common::MakeFormattedError<bool>( "'{}' could not be read: {}",
                                                     asset->GetMetadata().Filepath.string(), failure );
        asset->ResolveDependencies( manager );
        return BOOLSUCCESS;
    }
} // namespace Desert::Assets
