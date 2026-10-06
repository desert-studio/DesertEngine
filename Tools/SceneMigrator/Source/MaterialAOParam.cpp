#include "MaterialAOParam.hpp"

#include <Engine/Assets/MaterialFormat.hpp>

#include <algorithm>

namespace Desert::Migration
{
    Common::ResultStr<std::optional<std::string>> MaterialWithOneAOParam( const std::string& source,
                                                                          const std::string& text )
    {
        constexpr const char* kRetired = "OcclusionStrength";
        constexpr const char* kOneHome = "AOStrength";
        using Result                   = std::optional<std::string>;

        auto parsed = Assets::ParseMaterialJson( source, text );
        if ( !parsed )
            return Common::MakeError<Result>( parsed.GetError() );
        Assets::MaterialData material = parsed.ExtractValue();

        const auto named = [&material]( const char* name )
        { return std::ranges::find( material.Params, std::string( name ), &Assets::MaterialShaderParam::Name ); };
        const auto retired = named( kRetired );
        if ( retired == material.Params.end() )
            return Common::MakeSuccess( Result{} );
        if ( named( kOneHome ) != material.Params.end() )
            return Common::MakeError<Result>( "'" + source + "' states both " + kOneHome + " and " + kRetired +
                                              ", two values for the one Ambient Occlusion input: keep one by "
                                              "hand, then run the migrator again" );
        retired->Name = kOneHome;

        auto written = Assets::WriteMaterialJson( material );
        if ( !written )
            return Common::MakeError<Result>( "'" + source + "': " + written.GetError() );
        return Common::MakeSuccess( Result{ written.ExtractValue() } );
    }
} // namespace Desert::Migration
