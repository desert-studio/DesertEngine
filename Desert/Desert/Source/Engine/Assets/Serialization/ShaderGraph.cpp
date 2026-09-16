#include <Engine/Assets/Serialization/ShaderGraph.hpp>

#include <Common/Utilities/FileSystem.hpp>

#include <rflcpp/rfl/DefaultIfMissing.hpp>
#include <rflcpp/rfl/json.hpp>

#include <format>

namespace Desert::Assets::Serialization::ShaderGraph
{
    std::string Serialize( const Document& doc )
    {
        return rfl::json::write( doc );
    }

    Common::ResultStr<Document> ParseShaderGraph( const std::string& json )
    {
        // `DefaultIfMissing` and not a strict read: a graph written before a field existed must still
        // open, and every field on this document has a value that means what the old build meant by its
        // absence. What is NOT tolerated is malformed JSON — that is a file somebody is about to lose
        // work over, and the message is reflect-cpp's own so it names the offending member.
        auto parsed = rfl::json::read<Document, rfl::DefaultIfMissing>( json );
        if ( !parsed )
            return Common::MakeError<Document>( std::format( "bad .dgraph: {}", parsed.error().what() ) );

        return Common::MakeSuccess( std::move( parsed.value() ) );
    }

    Common::BoolResultStr SaveShaderGraphFile( const std::filesystem::path& path, const Document& doc )
    {
        std::error_code ec;
        if ( path.has_parent_path() )
        {
            std::filesystem::create_directories( path.parent_path(), ec );
        }

        // Atomic, for SaveControlRigFile's reason: a failed write must not cost the author the graph they
        // already had on disk.
        return Common::Utils::FileSystem::WriteContentToFileAtomic( path, Serialize( doc ) );
    }
} // namespace Desert::Assets::Serialization::ShaderGraph
