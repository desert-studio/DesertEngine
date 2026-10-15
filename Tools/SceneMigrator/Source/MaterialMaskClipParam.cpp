#include "MaterialMaskClipParam.hpp"

#include <Engine/Assets/MaterialFormat.hpp>

#include <algorithm>

namespace Desert::Migration
{
    Common::ResultStr<std::optional<std::string>> MaterialWithMaskedTemplate( const std::string& source,
                                                                              const std::string& text )
    {
        // The header GUIDs of Editor/Resources/Shaders/Programs/Surface/{FoliageSurface,StandardSurface,
        // StandardSurfaceMasked}.shader.
        constexpr const char* kFoliageSurfaceGuid  = "acb8d06bbe1a49dc95c2f5ebdddb4c33";
        constexpr const char* kStandardSurfaceGuid = "4f1cac6af403a010c792d835dd6f7d44";
        constexpr const char* kStandardMaskedGuid  = "7b1d37900d0349d284ba0faa36a13dc7";
        constexpr const char* kStandardMaskedPath = "engine:Shaders/Programs/Surface/StandardSurfaceMasked.shader";
        constexpr const char* kRetired            = "AlphaCutoff";
        constexpr const char* kOneHome            = "OpacityMaskClipValue";
        using Result                              = std::optional<std::string>;

        auto parsed = Assets::ParseMaterialJson( source, text );
        if ( !parsed )
            return Common::MakeError<Result>( parsed.GetError() );
        Assets::MaterialData material = parsed.ExtractValue();
        if ( !material.Shader )
            return Common::MakeSuccess( Result{} );
        const bool foliage  = material.Shader->Guid == kFoliageSurfaceGuid;
        const bool standard = material.Shader->Guid == kStandardSurfaceGuid;
        if ( !foliage && !standard )
            return Common::MakeSuccess( Result{} );

        const auto param = [&material]( const char* name )
        { return std::ranges::find( material.Params, std::string( name ), &Assets::MaterialShaderParam::Name ); };
        const auto texture = [&material]( const char* name )
        { return std::ranges::find( material.Textures, std::string( name ), &Assets::MaterialAssetRef::Name ); };
        const auto retired = param( kRetired );
        if ( retired != material.Params.end() && param( kOneHome ) != material.Params.end() )
            return Common::MakeFormattedError<Result>(
                 "'{}' states both {} and {}, two values for the one opacity-mask clip threshold: keep one by "
                 "hand, then run the migrator again",
                 source, kOneHome, kRetired );

        if ( foliage || ( retired != material.Params.end() && retired->Value.x > 0.0f ) )
        {
            if ( retired == material.Params.end() )
                return Common::MakeSuccess( Result{} );
            retired->Name = kOneHome;
            if ( standard )
                material.Shader = Assets::AssetGuidRef{ kStandardMaskedGuid, kStandardMaskedPath };
        }
        else
        {
            // An opaque StandardSurface: the mask's three entries have no Property to land on.
            const auto channel = param( "OpacityChannel" );
            const auto mask    = texture( "u_OpacityTexture" );
            if ( retired == material.Params.end() && channel == material.Params.end() &&
                 mask == material.Textures.end() )
                return Common::MakeSuccess( Result{} );
            std::erase_if( material.Params, []( const Assets::MaterialShaderParam& p )
                           { return p.Name == kRetired || p.Name == "OpacityChannel"; } );
            std::erase_if( material.Textures,
                           []( const Assets::MaterialAssetRef& t ) { return t.Name == "u_OpacityTexture"; } );
        }

        auto written = Assets::WriteMaterialJson( material );
        if ( !written )
            return Common::MakeFormattedError<Result>( "'{}': {}", source, written.GetError() );
        return Common::MakeSuccess( Result{ written.ExtractValue() } );
    }
} // namespace Desert::Migration
