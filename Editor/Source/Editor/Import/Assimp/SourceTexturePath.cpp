#include "SourceTexturePath.hpp"

#include <Editor/Import/TextureSourceFormats.hpp>

#include <algorithm>

namespace Desert::Editor
{
    std::filesystem::path NormalizeTextureReference( std::string reference )
    {
        std::replace( reference.begin(), reference.end(), '\\', '/' );
        return std::filesystem::path( reference );
    }

    std::filesystem::path FindSourceTexture( const std::filesystem::path& basePath, const std::string& reference )
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path  ref = NormalizeTextureReference( reference );

        const fs::path literal = ref.is_absolute() ? ref : ( basePath / ref ).lexically_normal();
        if ( fs::exists( literal, ec ) )
            return literal;

        const std::string stem   = ref.stem().string();
        const std::string name   = ref.filename().string();
        const fs::path    dirs[] = { basePath, basePath / "textures" };
        for ( const auto& d : dirs )
        {
            if ( fs::exists( d / name, ec ) ) // exact filename
                return d / name;
            // Same stem, different extension — tried in the shared priority order (lossless first; see
            // TextureSourceFormats.hpp for why, and for who else reads this list).
            for ( const char* e : kTextureSourceExtensions )
            {
                const fs::path cand = d / ( stem + e );
                if ( fs::exists( cand, ec ) )
                    return cand;
            }
        }
        return {};
    }
} // namespace Desert::Editor
