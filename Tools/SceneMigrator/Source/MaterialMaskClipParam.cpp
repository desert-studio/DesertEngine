#include "MaterialMaskClipParam.hpp"

#include <Engine/Assets/MaterialFormat.hpp>

#include <algorithm>

namespace Desert::Migration
{
    Common::ResultStr<std::optional<std::string>> FoliageMaterialWithMaskClipParam( const std::string& source,
                                                                                    const std::string& text )
    {
        // Editor/Resources/Shaders/Programs/Surface/FoliageSurface.shader's header GUID.
        constexpr const char* kFoliageSurfaceGuid = "acb8d06bbe1a49dc95c2f5ebdddb4c33";
        constexpr const char* kRetired            = "AlphaCutoff";
        constexpr const char* kOneHome            = "OpacityMaskClipValue";
        using Result                              = std::optional<std::string>;

        auto parsed = Assets::ParseMaterialJson( source, text );
        if ( !parsed )
            return Common::MakeError<Result>( parsed.GetError() );
        Assets::MaterialData material = parsed.ExtractValue();
        if ( !material.Shader || material.Shader->Guid != kFoliageSurfaceGuid )
            return Common::MakeSuccess( Result{} );

        const auto named = [&material]( const char* name )
        { return std::ranges::find( material.Params, std::string( name ), &Assets::MaterialShaderParam::Name ); };
        const auto retired = named( kRetired );
        if ( retired == material.Params.end() )
            return Common::MakeSuccess( Result{} );
        if ( named( kOneHome ) != material.Params.end() )
            return Common::MakeFormattedError<Result>(
                 "'{}' states both {} and {}, two values for the one opacity-mask clip threshold: keep one by "
                 "hand, then run the migrator again",
                 source, kOneHome, kRetired );
        retired->Name = kOneHome;

        auto written = Assets::WriteMaterialJson( material );
        if ( !written )
            return Common::MakeFormattedError<Result>( "'{}': {}", source, written.GetError() );
        return Common::MakeSuccess( Result{ written.ExtractValue() } );
    }
} // namespace Desert::Migration
