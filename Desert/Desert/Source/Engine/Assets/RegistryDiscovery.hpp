#pragma once

#include <Engine/Assets/AssetManager.hpp>
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
                 Common::Content::KindName( kind ), row->Path.string(), guid, static_cast<uint64_t>( handle ) );

        auto created = manager->CreateAsset<AssetType>( AssetPriority::Medium, row->Path,
                                                        /*loadAfterCreate=*/false );
        if ( !created )
            return Common::MakeFormattedError<Asset<AssetType>>( "{} '{}' (GUID {}) could not be created",
                                                                 Common::Content::KindName( kind ),
                                                                 row->Path.string(), guid );

        // The service keys its entry by the number it was asked for; a shell that adopted a DIFFERENT number
        // (a header whose GUID disagrees with the registry's) would be announced under it and never answer.
        if ( created->GetMetadata().Handle != handle )
            return Common::MakeFormattedError<Asset<AssetType>>(
                 "{} '{}' was asked for as {} but its header names {} (registry GUID {}); re-cook the registry",
                 Common::Content::KindName( kind ), row->Path.string(), static_cast<uint64_t>( handle ),
                 static_cast<uint64_t>( created->GetMetadata().Handle ), guid );

        return Common::MakeSuccess( std::move( created ) );
    }
} // namespace Desert::Assets
