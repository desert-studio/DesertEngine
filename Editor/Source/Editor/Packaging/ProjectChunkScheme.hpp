#pragma once

#include <Common/Content/ChunkSchemeSession.hpp>

namespace Desert::Editor
{
    // THE EDITOR'S ONLY ChunkSchemeSession: the Build Settings panel, the Content Chunks panel and the
    // palette commands all act on the object this returns, so an action taken through any of them is
    // what the others draw next frame. Bound to Common::Content::ChunkSchemePath() of the project open
    // NOW — a project switch re-binds (and re-reads) rather than showing the previous project's file.
    Common::Content::ChunkSchemeSession& ProjectChunkScheme();
} // namespace Desert::Editor
