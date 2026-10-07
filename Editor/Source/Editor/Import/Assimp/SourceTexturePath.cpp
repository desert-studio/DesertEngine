#include "SourceTexturePath.hpp"

#include <Editor/Import/TextureSourceFormats.hpp>

#include <Common/Core/Constants.hpp>

#include <algorithm>

namespace Desert::Editor
{
    std::filesystem::path NormalizeTextureReference( std::string reference )
    {
        std::replace( reference.begin(), reference.end(), '\\', '/' );
        return { std::move( reference ) };
    }

    std::filesystem::path FindSourceTexture( const std::filesystem::path& basePath, const std::string& reference )
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path  ref = NormalizeTextureReference( reference );
        // A candidate keeps the source's spelling (a project key stays a key); whether it exists is asked of
        // the project (FullPath), the root the source itself was read from - never of the working directory.
        const auto exists = [&ec]( const fs::path& candidate )
        { return fs::exists( Common::Constants::Path::FullPath( candidate ), ec ); };

        fs::path literal = ref.is_absolute() ? ref : ( basePath / ref ).lexically_normal();
        if ( exists( literal ) )
            return literal;

        const std::string stem   = ref.stem().string();
        const std::string name   = ref.filename().string();
        const fs::path    dirs[] = { basePath, basePath / "textures" };
        for ( const auto& d : dirs )
        {
            if ( exists( d / name ) ) // exact filename
                return d / name;
            // Same stem, different extension — tried in the shared priority order (lossless first; see
            // TextureSourceFormats.hpp for why, and for who else reads this list).
            for ( const char* e : kTextureSourceExtensions )
            {
                const fs::path cand = d / ( stem + e );
                if ( exists( cand ) )
                    return cand;
            }
        }
        return {};
    }
} // namespace Desert::Editor
