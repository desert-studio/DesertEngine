#include "ShaderLocatorFollow.hpp"

#include <Common/Core/Constants.hpp>

#include <format>
#include <regex>
#include <utility>

namespace Desert::Migration
{
    ShaderLocatorFollow FollowShaderLocator( const std::string&                        source,
                                             const std::map<std::string, std::string>& engineShaders )
    {
        static const std::regex kShaderRef(
             R"re("Shader"\s*:\s*\{\s*"Guid"\s*:\s*"([0-9a-f]{32})"\s*,\s*"Path"\s*:\s*"(engine:[^"]*)")re" );
        std::smatch match;
        if ( !std::regex_search( source, match, kShaderRef ) )
            return {};
        const std::string guid    = match[1].str();
        const std::string locator = match[2].str();
        const auto        found   = engineShaders.find( guid );
        if ( found == engineShaders.end() )
            return { .Error = std::format( "shader {} at '{}' is stated by no engine shader under {}", guid,
                                           locator, Common::Constants::Path::SHADERDIR_PATH.string() ),
                     .Text  = std::nullopt };
        if ( found->second == locator )
            return {};
        std::string text = source;
        text.replace( static_cast<std::size_t>( match.position( 2 ) ), locator.size(), found->second );
        return { .Error = std::string(), .Text = std::move( text ) };
    }
} // namespace Desert::Migration
