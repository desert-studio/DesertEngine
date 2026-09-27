#include "ImportUnits.hpp"

#include <Common/Core/Units.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace Desert::Editor::ImportUnits
{
    namespace
    {
        std::string LowerExtension( std::string_view extension )
        {
            std::string lowered( extension );
            std::transform( lowered.begin(), lowered.end(), lowered.begin(),
                            []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
            return lowered;
        }

        // glTF 2.0 §3.5: "The units for all linear distances are meters." The three extensions are the
        // GLTF row of BuildScripts/ThirdParty/AssimpImporters.txt — assimp's glTF2 importer claims `vrm`
        // alongside `gltf`/`glb`, so a .vrm arrives through the same reader and carries the same unit.
        bool IsGltfFamily( const std::string& lowered )
        {
            return lowered == ".gltf" || lowered == ".glb" || lowered == ".vrm";
        }
    } // namespace

    bool IsUsableScale( float scale )
    {
        return std::isfinite( scale ) && scale > 0.0f;
    }

    Scale Resolve( std::string_view extension, bool fileStatedUnit, float statedCentimetresPerUnit )
    {
        if ( fileStatedUnit )
        {
            if ( !IsUsableScale( statedCentimetresPerUnit ) )
                return { Common::Units::UnitsPerCm, Source::StatedButUnusable };

            return { statedCentimetresPerUnit, Source::StatedByFile };
        }

        if ( IsGltfFamily( LowerExtension( extension ) ) )
            return { Common::Units::UnitsPerMetre, Source::FixedByFormat };

        // OBJ, and anything else that arrives without a word about its unit. Taken as centimetres —
        // the engine's own unit — rather than as metres, which is what assimp would have assumed.
        return { Common::Units::UnitsPerCm, Source::AssumedCentimetres };
    }

    float GlobalScaleFactor( float centimetresPerUnit, float assimpMetresPerUnit )
    {
        return centimetresPerUnit / assimpMetresPerUnit;
    }

    const char* Describe( Source source )
    {
        switch ( source )
        {
            case Source::StatedByFile:
                return "stated by the file";
            case Source::FixedByFormat:
                return "fixed by the format";
            case Source::AssumedCentimetres:
                return "assumed: the file states no unit";
            case Source::StatedButUnusable:
                return "the file stated a unit that is not a usable scale";
        }
        return "unknown";
    }
} // namespace Desert::Editor::ImportUnits
