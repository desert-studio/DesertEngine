#include "GlslEmit.hpp"
#include "TwinCheck.hpp"

#include <format>

namespace Desert::ShaderProtocolGen
{
    namespace
    {
        std::string Json( const std::string_view text )
        {
            std::string out = "\"";
            for ( const char c : text )
            {
                if ( c == '"' || c == '\\' )
                    out += '\\';
                if ( c == '\n' )
                    out += "\\n";
                else if ( static_cast<unsigned char>( c ) < 0x20 )
                    out += std::format( "\\u{:04x}", static_cast<int>( c ) );
                else
                    out += c;
            }
            return out + "\"";
        }
    } // namespace

    // {"includes":[{path, source_header, hash, structs:[{glsl, members}]}], "twins":[...], "unused_allow":[...]}:
    // scripts/Dev/arch_census.py reads `twins` and `unused_allow` for the shader-twin rule.
    std::string EmitManifest( const std::span<const GeneratedInclude> includes,
                              const std::span<const GeneratedShape>   shapes,
                              const std::span<const TwinFinding>      twins,
                              const std::span<const TwinAllow>        allowList )
    {
        std::string out = "{\n  \"includes\": [";
        for ( size_t i = 0; i < includes.size(); ++i )
        {
            const GeneratedInclude& include = includes[i];
            out += std::format(
                 "{}\n    {{\"path\": {}, \"source_header\": {}, \"hash\": \"{:016x}\", \"structs\": [",
                 i == 0 ? "" : ",", Json( include.Decl->Path ), Json( include.Decl->SourceHeader ), include.Hash );
            bool first = true;
            for ( const GeneratedShape& shape : shapes )
            {
                if ( shape.Path != include.Decl->Path )
                    continue;
                out += std::format( "{}{{\"glsl\": {}, \"members\": [", first ? "" : ", ", Json( shape.Name ) );
                for ( size_t m = 0; m < shape.Members.size(); ++m )
                    out += ( m == 0 ? "" : ", " ) + Json( shape.Members[m] );
                out += "]}";
                first = false;
            }
            out += "]}";
        }
        out += "\n  ],\n  \"twins\": [";
        for ( size_t i = 0; i < twins.size(); ++i )
        {
            const TwinFinding& twin = twins[i];
            out += std::format( "{}\n    {{\"path\": {}, \"line\": {}, \"name\": {}, \"generated\": {}, "
                                "\"source_header\": {}, \"rule\": \"{}\"}}",
                                i == 0 ? "" : ",", Json( twin.Path ), twin.Line, Json( twin.Name ),
                                Json( twin.GeneratedPath ), Json( twin.SourceHeader ),
                                twin.SameName ? "name" : "members" );
        }
        out += "\n  ],\n  \"unused_allow\": [";
        bool first = true;
        for ( const TwinAllow& allow : allowList )
        {
            if ( allow.Used )
                continue;
            out += std::format( "{}\n    {{\"path\": {}, \"name\": {}, \"line\": {}}}", first ? "" : ",",
                                Json( allow.Path ), Json( allow.Name ), allow.Line );
            first = false;
        }
        out += "\n  ]\n}\n";
        return out;
    }
} // namespace Desert::ShaderProtocolGen
