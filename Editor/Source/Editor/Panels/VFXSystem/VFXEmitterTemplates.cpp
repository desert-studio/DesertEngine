#include "VFXEmitterTemplates.hpp"

#include <Common/Core/Constants.hpp>

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>
#include <system_error>

namespace Desert::Editor
{
    namespace S = Assets::Serialization;

    std::filesystem::path VFXEmitterTemplateDir()
    {
        return Common::Constants::Path::RESOURCE_PATH / "VFX" / "EmitterTemplates";
    }

    Common::ResultStr<std::vector<VFXEmitterTemplate>> ReadVFXEmitterTemplates( const std::filesystem::path& dir )
    {
        using Result    = std::vector<VFXEmitterTemplate>;
        auto categories = S::ReadVFXCategories( dir / S::kVFXCategoriesFileName );
        if ( !categories )
            return Common::MakeFormattedError<Result>( "emitter templates: {}", categories.GetError() );

        std::error_code                    ec;
        std::vector<std::filesystem::path> files;
        for ( const auto& entry : std::filesystem::directory_iterator( dir, ec ) )
            if ( entry.is_regular_file() && entry.path().extension() == S::kVFXSystemExtension )
                files.push_back( entry.path() );
        if ( ec )
            return Common::MakeFormattedError<Result>( "emitter templates: '{}' could not be listed: {}",
                                                       dir.string(), ec.message() );
        std::ranges::sort( files );

        Result templates;
        for ( const auto& file : files )
        {
            std::ifstream     in( file, std::ios::binary );
            const std::string text( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );
            auto              parsed = S::ParseVFXSystem( text, categories.GetValue() );
            if ( !parsed )
                return Common::MakeFormattedError<Result>( "emitter template '{}': {}", file.string(),
                                                           parsed.GetError() );
            const S::VFXSystemData system = parsed.ExtractValue();
            const std::string      stem   = file.stem().string();
            for ( const S::VFXEmitterData& emitter : system.Emitters )
                templates.push_back( VFXEmitterTemplate{
                     system.Emitters.size() == 1 ? stem : std::format( "{} / {}", stem, emitter.Name ), emitter } );
        }
        return Common::MakeSuccess( std::move( templates ) );
    }
} // namespace Desert::Editor
