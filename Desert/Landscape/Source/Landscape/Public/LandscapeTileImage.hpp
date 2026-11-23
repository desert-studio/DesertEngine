#pragma once

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <memory>
#include <utility>

namespace Desert::ECS
{
    // What RefreshLandscapeTileImage did to a tile's GPU copy.
    enum class LandscapeTileImageUpdate : uint8_t
    {
        Kept,      // fresh: nothing written
        Rewritten, // same image, new texels
        Created,   // a new image (there was none, or its size moved)
    };

    // A landscape tile's GPU copies (heightmap, weightmap) live as long as the tile at their size, and an
    // edit is written INTO them - UE's pattern: FLandscapeEditDataInterface::SetHeightData writes the
    // component's existing heightmap texture through FLandscapeTextureDataInfo::UpdateTextureData
    // (UpdateTextureRegions), it never makes a new one per edit.
    //
    // Why it is load-bearing here: the image's address is part of TerrainTextureKey, and TerrainRenderer keeps
    // one set of materials per key. A new image per sculpt upload was a new set of three materials per dirty
    // tile per frame of sculpting, never released (L8-leak). So: kept while fresh, rewritten in place while
    // its size holds, created only when there is none or the tile's sample count changed.
    //
    // `makeTexels()` builds the payload, and only when something is written; `create( texels )` makes a new
    // image from it and may return null. A template over the image type so the rule runs without a device.
    template <class Image, class MakeTexels, class Create>
    Common::ResultStr<LandscapeTileImageUpdate>
    RefreshLandscapeTileImage( std::shared_ptr<Image>& image, uint32_t& width, uint32_t& height,
                               uint32_t wantWidth, uint32_t wantHeight, bool stale, MakeTexels&& makeTexels,
                               Create&& create )
    {
        const bool sameSize = image && width == wantWidth && height == wantHeight;
        if ( sameSize && !stale )
            return Common::MakeSuccess( LandscapeTileImageUpdate::Kept );

        auto texels = makeTexels();
        if ( sameSize )
        {
            const Common::BoolResultStr written = image->SetData( std::move( texels ) );
            if ( !written.IsSuccess() )
                return Common::MakeFormattedError<LandscapeTileImageUpdate>(
                     "the {} x {} image could not be rewritten in place: {}", width, height, written.GetError() );
            return Common::MakeSuccess( LandscapeTileImageUpdate::Rewritten );
        }

        image  = create( std::move( texels ) );
        width  = wantWidth;
        height = wantHeight;
        if ( !image )
            return Common::MakeFormattedError<LandscapeTileImageUpdate>( "the {} x {} image could not be created",
                                                                         width, height );
        return Common::MakeSuccess( LandscapeTileImageUpdate::Created );
    }
} // namespace Desert::ECS
