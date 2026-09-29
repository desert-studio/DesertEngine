#include <Editor/Import/TextureChannelPack.hpp>

// STB_IMAGE(_WRITE)_IMPLEMENTATION is compiled into stb_image.cpp; declarations only here.
#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
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
            int                                           Width  = 0;
            int                                           Height = 0;
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
        return slot.Parts.front().Source.parent_path() / std::format( "{}_{}.png", name, slotName );
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
            image.Data.reset(
                 stbi_load( part.Source.string().c_str(), &image.Width, &image.Height, &components, 4 ) );
            if ( !image.Data )
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
            const uint8_t*         source   = images[p].Data.get();
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

        const auto written = WriteDerivedTexture( encoded, out );
        if ( !written.IsSuccess() )
            return Common::MakeError<PackOutcome>(
                 std::format( "[Import] slot '{}': {}", slot.Slot, written.GetError() ) );
        return written;
    }

    Common::ResultStr<PackOutcome> WriteDerivedTexture( std::string_view bytes, const std::filesystem::path& out )
    {
        {
            std::ifstream existing( out, std::ios::binary );
            if ( existing && std::string( std::istreambuf_iterator<char>( existing ),
                                          std::istreambuf_iterator<char>() ) == bytes )
                return Common::MakeSuccess( PackOutcome::Unchanged );
        }
        std::ofstream file( out, std::ios::binary | std::ios::trunc );
        file.write( bytes.data(), static_cast<std::streamsize>( bytes.size() ) );
        if ( !file.good() )
            return Common::MakeError<PackOutcome>( std::format( "cannot write '{}'", out.generic_string() ) );
        return Common::MakeSuccess( PackOutcome::Written );
    }
} // namespace Desert::Editor
