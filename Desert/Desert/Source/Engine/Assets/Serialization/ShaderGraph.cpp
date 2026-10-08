#include <Engine/Assets/Serialization/ShaderGraph.hpp>

#include <Common/Json/Json.hpp>

#include <format>

namespace Desert::Assets::Serialization::ShaderGraph
{
    std::string Serialize( const Document& doc )
    {
        return Common::Json::Write( doc );
    }

    Common::ResultStr<Document> ParseShaderGraph( const std::string& json )
    {
        // Json::Read's rule (owner 2026-10-07): a field the graph does not state is the struct's own default; a
        // field this build does not know is refused with its path, instead of being dropped on the next save.
        auto parsed = Common::Json::Read<Document>( json );
        if ( !parsed )
            return Common::MakeError<Document>( std::format( "bad .dgraph: {}", parsed.GetError() ) );

        return Common::MakeSuccess( parsed.ExtractValue() );
    }

} // namespace Desert::Assets::Serialization::ShaderGraph
