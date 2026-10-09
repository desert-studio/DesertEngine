#include <Editor/Import/TextureChannelPack.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>
#include <Editor/Import/DdsSource.hpp>
#include <Engine/Assets/TextureSourceAsset.hpp>

// STB_IMAGE(_WRITE)_IMPLEMENTATION is compiled into stb_image.cpp; declarations only here.
#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace Desert::Editor
{
    namespace
    {
        constexpr std::string_view kChannels = "rgba";

        struct Pixels
        {
            std::unique_ptr<uint8_t, void ( * )( void* )> Data{ nullptr, stbi_image_free };
            std::vector<unsigned char>                    Owned; // a DDS's decoded texels (stb owns no buffer)
            int                                           Width  = 0;
            int                                           Height = 0;
            [[nodiscard]] const uint8_t*                  Texels() const
            {
                return Data ? Data.get() : Owned.data();
            }
        };
    } // namespace

    std::filesystem::path PackedTexturePath( const ImportedTextureSlot& slot )
    {
        std::string                        name;
        std::vector<std::filesystem::path> seen;
        for ( const auto& part : slot.Parts )
        {
            if ( std::ranges::find( seen, part.Source ) != seen.end() )
                continue;
            seen.push_back( part.Source );
            name += ( name.empty() ? "" : "+" ) + part.Source.stem().string();
        }
        std::string_view slotName = slot.Slot;
        if ( slotName.starts_with( "u_" ) )
            slotName.remove_prefix( 2 );
        return slot.Parts.front().Source.parent_path() /
               std::format( "{}_{}{}", name, slotName, Assets::kTextureAssetExtension );
    }

    Common::ResultStr<PackOutcome> PackTextureChannels( const ImportedTextureSlot&   slot,
                                                        const std::filesystem::path& out )
    {
        if ( slot.Parts.empty() )
            return Common::MakeError<PackOutcome>(
                 std::format( "[Import] slot '{}' has nothing to pack", slot.Slot ) );

        std::vector<Pixels> images;
        for ( const auto& part : slot.Parts )
        {
            Pixels& image      = images.emplace_back();
            int     components = 0;
            const auto bytes = Assets::ReadTextureSourceImage( Common::Constants::Path::FullPath( part.Source ) );
            if ( !bytes.IsSuccess() )
                return Common::MakeError<PackOutcome>( std::format( "[Import] slot '{}': cannot read '{}' ({})",
                                                                    slot.Slot, part.Source.generic_string(),
                                                                    bytes.GetError() ) );
            const std::vector<std::byte>& raw = bytes.GetValue();
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - the file's bytes viewed as characters
            const std::string_view rawText( reinterpret_cast<const char*>( raw.data() ), raw.size() );
            // A DDS (a Lumberyard Bistro / ORCA packed map) is decoded by the one DDS reader, as the texture cook
            // does; stb has no DDS decoder and would refuse it as an "unknown image type".
            if ( IsDdsSource( rawText, part.Source.generic_string() ) )
            {
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - bytes viewed as unsigned char
                auto dds = DecodeDdsSource( reinterpret_cast<const unsigned char*>( raw.data() ), raw.size() );
                if ( !dds.IsSuccess() || dds.GetValue().IsFloat )
                    return Common::MakeError<PackOutcome>( std::format(
                         "[Import] slot '{}': cannot read '{}' ({})", slot.Slot, part.Source.generic_string(),
                         dds.IsSuccess() ? "a float DDS is not a packable 8-bit map" : dds.GetError() ) );
                DdsSourceImage decoded = dds.ExtractValue();
                image.Width            = static_cast<int>( decoded.Width );
                image.Height           = static_cast<int>( decoded.Height );
                image.Owned            = std::move( decoded.Rgba8 );
            }
            else
                // stb_image's C API takes `const stbi_uc*` (unsigned char), which may view any object's bytes;
                // the bytes reach it only through this cast.
                image.Data.reset( stbi_load_from_memory(
                     reinterpret_cast<const stbi_uc*>(
                          bytes.GetValue().data() ), // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                     static_cast<int>( bytes.GetValue().size() ), &image.Width, &image.Height, &components, 4 ) );
            if ( !image.Data && image.Owned.empty() )
                return Common::MakeError<PackOutcome>( std::format( "[Import] slot '{}': cannot read '{}' ({})",
                                                                    slot.Slot, part.Source.generic_string(),
                                                                    stbi_failure_reason() ) );
            if ( image.Width != images.front().Width || image.Height != images.front().Height )
                return Common::MakeError<PackOutcome>( std::format(
                     "[Import] slot '{}': '{}' is {}x{} and '{}' is {}x{}; packing needs one size", slot.Slot,
                     slot.Parts.front().Source.generic_string(), images.front().Width, images.front().Height,
                     part.Source.generic_string(), image.Width, image.Height ) );
        }

        const int            width  = images.front().Width;
        const int            height = images.front().Height;
        std::vector<uint8_t> packed( static_cast<std::size_t>( width ) * static_cast<std::size_t>( height ) * 4,
                                     255 );
        for ( std::size_t p = 0; p < slot.Parts.size(); ++p )
        {
            const std::string_view channels = slot.Parts[p].Channels.empty() ? kChannels : slot.Parts[p].Channels;
            const uint8_t*         source   = images[p].Texels();
            for ( const char c : channels )
                for ( std::size_t i = kChannels.find( c ); i < packed.size(); i += 4 )
                    packed[i] = source[i];
        }
        std::string encoded;
        const auto  append = []( void* context, void* data, int size )
        {
            static_cast<std::string*>( context )->append( static_cast<const char*>( data ),
                                                          static_cast<std::size_t>( size ) );
        };
        if ( stbi_write_png_to_func( append, &encoded, width, height, 4, packed.data(), width * 4 ) == 0 )
            return Common::MakeError<PackOutcome>(
                 std::format( "[Import] slot '{}': cannot encode '{}'", slot.Slot, out.generic_string() ) );

        auto written = WriteDerivedTexture( encoded, out, ".png" );
        if ( !written.IsSuccess() )
            return Common::MakeError<PackOutcome>(
                 std::format( "[Import] slot '{}': {}", slot.Slot, written.GetError() ) );
        return written;
    }

    Common::ResultStr<PackOutcome> WriteDerivedTexture( std::string_view bytes, const std::filesystem::path& asset,
                                                        std::string_view imageExtension )
    {
        // `asset` is a project key (relative) or absolute; it lands off the project (FullPath), the root the
        // texture importer reads that key from - never off the working directory. The provenance key names the
        // image the bytes are (`<asset stem><imageExtension>` beside it): its extension is what the cook reads
        // the format from, and nothing ever opens it as a file.
        const std::filesystem::path onDisk = Common::Constants::Path::FullPath( asset );
        std::filesystem::path       image  = onDisk;
        image.replace_extension( imageExtension );
        const auto view    = std::as_bytes( std::span( bytes ) );
        const auto written = Assets::WriteTextureSource( onDisk, Common::Content::ContentKind::Texture,
                                                         Common::AssetHandle::StableKeyForPath( image ),
                                                         std::vector<std::byte>( view.begin(), view.end() ), {} );
        if ( !written.IsSuccess() )
            return Common::MakeError<PackOutcome>( std::format( "cannot write the texture asset '{}': {}",
                                                                asset.generic_string(), written.GetError() ) );
        return Common::MakeSuccess( written.GetValue() == Assets::TextureSourceWrite::Unchanged
                                         ? PackOutcome::Unchanged
                                         : PackOutcome::Written );
    }
} // namespace Desert::Editor
