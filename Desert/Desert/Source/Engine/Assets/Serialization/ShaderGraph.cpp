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
        // STRICT (owner rule: a missing field is an error, not a fallback): a graph that lacks a field or
        // states one this build does not know is refused with the field's path, instead of opening with a
        // value nobody authored. A field added later is std::optional or moved into the files by migration.
        auto parsed = Common::Json::Read<Document>( json );
        if ( !parsed )
            return Common::MakeError<Document>( std::format( "bad .dgraph: {}", parsed.GetError() ) );

        return Common::MakeSuccess( std::move( parsed.GetValue() ) );
    }

} // namespace Desert::Assets::Serialization::ShaderGraph
