#include "GrassCoverLookBuilder.hpp"

#include <Editor/Import/TextureImporter.hpp>

#include <optional>
#include <vector>

namespace Desert::Editor
{
    Common::ResultStr<std::string> BuildGrassCoverLook( const Assets::GrassCoverLookInput& input )
    {
        // 256 texels across: a blade card of a 2k atlas keeps a few dozen texels of its width, enough for the
        // mean of its veins and dry tips; the level is the cooked one, so the mean is what the mip chain draws.
        constexpr uint32_t kGrassCoverLookEdge = 256;
        auto               albedo = TextureImporter::DecodePlatformMips( input.AlbedoAsset, kGrassCoverLookEdge );
        if ( !albedo.IsSuccess() )
            return Common::MakeError<std::string>( albedo.GetError() );
        if ( albedo.GetValue().empty() )
            return Common::MakeFormattedError<std::string>( "albedo map '{}' has no level within {} texels",
                                                            input.AlbedoAsset.generic_string(),
                                                            kGrassCoverLookEdge );
        const TextureImporter::PlatformMip&                      a = albedo.GetValue().front();
        std::optional<std::vector<TextureImporter::PlatformMip>> maskMips;
        std::optional<Assets::GrassCoverLookLevel>               maskLevel;
        if ( !input.MaskAsset.empty() )
        {
            auto mask = TextureImporter::DecodePlatformMips( input.MaskAsset, kGrassCoverLookEdge );
            if ( !mask.IsSuccess() )
                return Common::MakeError<std::string>( mask.GetError() );
            if ( mask.GetValue().empty() )
                return Common::MakeFormattedError<std::string>( "opacity mask '{}' has no level within {} texels",
                                                                input.MaskAsset.generic_string(),
                                                                kGrassCoverLookEdge );
            maskMips                              = mask.ExtractValue();
            const TextureImporter::PlatformMip& m = maskMips->front();
            maskLevel                             = Assets::GrassCoverLookLevel{ m.Width, m.Height, m.Texels };
        }
        const Assets::GrassCoverLook look =
             Assets::AverageGrassCoverLook( input, { a.Width, a.Height, a.Texels }, maskLevel );
        if ( look.Coverage <= 0.0f )
            return Common::MakeFormattedError<std::string>(
                 "albedo map '{}': no texel of its {} rect(s) passes the opacity mask '{}' (clip {})",
                 input.AlbedoAsset.generic_string(), input.Rects.size(), input.MaskAsset.generic_string(),
                 input.MaskClip );
        return Common::MakeSuccess( Assets::EncodeGrassCoverLook( look ) );
    }

} // namespace Desert::Editor
