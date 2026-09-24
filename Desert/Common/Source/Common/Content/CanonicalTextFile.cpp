// WriteCanonicalJsonFileAtomic lives in its own translation unit on purpose: it reaches FileSystem, and
// FileSystem's object carries the Cocoa file dialogs. Kept inside CanonicalText.cpp, every suite that
// only lays out text would have had to link AppKit.
#include "CanonicalText.hpp"

#include <Common/Utilities/FileSystem.hpp>

namespace Common::Content
{
    ResultStr<bool> WriteCanonicalJsonFileAtomic( const std::filesystem::path& file, std::string_view json )
    {
        const auto text = CanonicalJsonTextOfWriterOutput( json );
        if ( !text )
            return MakeFormattedError<bool>( "save of '{}' refused, the file is unchanged: {}", file.string(),
                                             text.GetError() );
        return Utils::FileSystem::WriteContentToFileAtomic( file, text.GetValue() );
    }
} // namespace Common::Content
