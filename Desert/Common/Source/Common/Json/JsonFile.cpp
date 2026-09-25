// The file half of the facade lives in its own translation unit for the reason CanonicalTextFile.cpp gives:
// it reaches FileSystem, whose object carries the Cocoa file dialogs, and a suite that only turns structs into
// text must not have to link AppKit for it.
#include "Json.hpp"

#include <Common/Content/CanonicalText.hpp>
#include <Common/Utilities/FileSystem.hpp>

namespace Common::Json::Detail
{
    ResultStr<std::string> ReadFileText( const std::filesystem::path& file )
    {
        auto text = Utils::FileSystem::ReadFileContent( file );
        if ( !text )
            return MakeFormattedError<std::string>( "{}: could not be read: {}", file.string(), text.GetError() );
        return MakeSuccess( text.ExtractValue() );
    }

    BoolResultStr WriteTextAtomic( const std::filesystem::path& file, const std::string& json )
    {
        auto written = Content::WriteCanonicalJsonFileAtomic( file, json );
        if ( !written )
            return MakeFormattedError<bool>( "{}: {}", file.string(), written.GetError() );
        return written;
    }
} // namespace Common::Json::Detail
