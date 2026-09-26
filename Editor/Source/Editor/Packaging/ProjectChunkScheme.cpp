#include "ProjectChunkScheme.hpp"

#include <Common/Content/ContentChunks.hpp>

#include <optional>

namespace Desert::Editor
{
    Common::Content::ChunkSchemeSession& ProjectChunkScheme()
    {
        static std::optional<Common::Content::ChunkSchemeSession> s_Session;
        const std::filesystem::path                               path = Common::Content::ChunkSchemePath();
        if ( !s_Session || s_Session->Path() != path )
            s_Session.emplace( path );
        return *s_Session;
    }
} // namespace Desert::Editor
