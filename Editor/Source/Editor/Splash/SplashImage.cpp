#include "SplashImage.hpp"

#include <Common/Utilities/FileSystem.hpp>
#include <Engine/Assets/Serialization/TextureBinary.hpp>
#include <Engine/Core/Formats/BlockCompression.hpp>

#include <chrono>
#include <string_view>

namespace Desert::Editor::Splash
{
    Common::ResultStr<SplashPixels> LoadSplashPixels( const std::filesystem::path& cookedTex )
    {
        namespace Fmt = ::Desert::Core::Formats;

        const auto start = std::chrono::steady_clock::now();

        // ABSENCE IS ASKED FIRST, and not left to the read: the reader logs its own error line for a missing
        // file, and the first start on a fresh clone is expected to be missing this one. The splash owes
        // the log ONE line saying why it has no picture, and it is the caller's (SplashScreen*::Run).
        std::error_code ec;
        if ( !std::filesystem::is_regular_file( cookedTex, ec ) )
            return Common::MakeFormattedError<SplashPixels>( "'{}' is not there yet; it is cooked by the editor's "
                                                             "'Cooking the splash picture' stage for the next "
                                                             "start, and by scripts/MacOS/Package.sh",
                                                             cookedTex.string() );

        auto bytes = Common::Utils::FileSystem::ReadFileContent( cookedTex );
        if ( !bytes.IsSuccess() )
            return Common::MakeFormattedError<SplashPixels>( "'{}' could not be read: {}", cookedTex.string(),
                                                             bytes.GetError() );

        const std::string whatFor = cookedTex.string();
        auto decoded = Assets::Serialization::DecodeTextureBinary( std::string_view( bytes.GetValue() ), whatFor );
        if ( !decoded.IsSuccess() )
            return Common::MakeError<SplashPixels>( decoded.GetError() );

        const auto& data = decoded.GetValue();
        if ( data.Levels.empty() )
            return Common::MakeFormattedError<SplashPixels>( "'{}' holds no image level", whatFor );

        const auto&          level  = data.Levels[0];
        const unsigned char* source = data.Pixels.data() + level.ByteOffset;
        const std::size_t    size   = static_cast<std::size_t>( level.ByteSize );

        SplashPixels pixels;
        pixels.Width  = level.Width;
        pixels.Height = level.Height;

        if ( data.Format == Fmt::ImageFormat::RGBA8F )
        {
            // An uncooked-to-blocks texture (a `.detex` that stopped saying Colour) is still a correct
            // picture, just a larger file; it is taken as it is rather than refused.
            pixels.Rgba.assign( source, source + size );
        }
        else
        {
            if ( Fmt::SourceFormatFor( data.Format ) != Fmt::ImageFormat::RGBA8F )
                return Common::MakeFormattedError<SplashPixels>(
                     "'{}' is format {}, which does not decode to 8-bit colour; the splash is authored as "
                     "Colour (Splash.detex)",
                     whatFor, static_cast<uint32_t>( data.Format ) );

            auto rgba = Fmt::BlockDecompressImage( level.Width, level.Height, data.Format, Fmt::ImageFormat::RGBA8F,
                                                   source, size );
            if ( !rgba.IsSuccess() )
                return Common::MakeFormattedError<SplashPixels>( "'{}' could not be decoded: {}", whatFor,
                                                                 rgba.GetError() );
            pixels.Rgba = rgba.ExtractValue();
        }

        if ( pixels.Rgba.size() != static_cast<std::size_t>( pixels.Width ) * pixels.Height * 4u )
            return Common::MakeFormattedError<SplashPixels>( "'{}' decoded to {} bytes for a {}x{} image",
                                                             whatFor, pixels.Rgba.size(), pixels.Width,
                                                             pixels.Height );

        pixels.DecodeMs =
             std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - start ).count();
        return Common::MakeSuccess( std::move( pixels ) );
    }
} // namespace Desert::Editor::Splash
