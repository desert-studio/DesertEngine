#include <Editor/LiveCoding/LiveCodingTypes.hpp>

#include <format>

namespace Desert::Editor::LiveCoding
{
    namespace
    {
        // "HistoryPanel.cpp, VFXWorld.cpp": the file names alone, the full paths are in the logged report.
        std::string FileNames( const std::vector<std::filesystem::path>& files )
        {
            std::string names;
            for ( const std::filesystem::path& file : files )
            {
                if ( !names.empty() )
                    names += ", ";
                names += file.filename().string();
            }
            return names;
        }

        const char* Plural( const std::size_t count, const char* one, const char* many )
        {
            return count == 1 ? one : many;
        }
    } // namespace

    std::string PatchReport::Summary() const
    {
        // UNMATCHED = NewFunctions: a function the patch defines that has no address in the image's map. A
        // function the edit added and a function whose name failed to match the map are the same fact here,
        // so it is named for what the acceptance reads it as (Live++ prints the same count as "unmatched").
        std::string summary = std::format( "patched {} {} in {} {} ({})", Redirected.size(),
                                           Plural( Redirected.size(), "function", "functions" ), Edited.size(),
                                           Plural( Edited.size(), "source", "sources" ), FileNames( Edited ) );
        if ( !Inliners.empty() )
            summary += std::format( " (+{} {}: {})", Inliners.size(),
                                    Plural( Inliners.size(), "inliner", "inliners" ), FileNames( Inliners ) );
        summary += std::format( ", {} unmatched, {} new data", NewFunctions.size(), NewData.size() );
        if ( !NotRerun.empty() )
            summary += std::format( ", {} {} in effect after a restart", NotRerun.size(),
                                    Plural( NotRerun.size(), "initialiser", "initialisers" ) );
        summary += std::format( ", compile {:.1f} s, apply {:.1f} ms", CompileSeconds, ApplyMilliseconds );
        return summary;
    }
} // namespace Desert::Editor::LiveCoding
