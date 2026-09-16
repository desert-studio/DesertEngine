#pragma once

// DELIBERATELY NOT <Editor/Core/SubjectOpenRequest.hpp>, for CloudDocumentOpen.hpp's reason: that header
// opens `namespace Desert::Editor::Core`, and this one is included by panels that spell Desert::Core as an
// unqualified `Core::` from inside Desert::Editor. The queueing is declared here and DEFINED in the
// matching .cpp so nothing in this header drags that namespace along.

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/ShaderGraphAsset.hpp>

#include <Common/Core/Logger.hpp>

#include <filesystem>
#include <string>
#include <system_error>

namespace Desert::Editor
{
    // Hands a resolved shader-graph handle to Core::SubjectOpenRequests. Declared here, defined in the
    // matching .cpp — see the note at the top of this file.
    void QueueShaderGraphSubjectOpen( const Assets::AssetHandle& subject );

    // "Open the `.dgraph` at this PATH in whatever edits it."
    //
    // The shader-graph twin of [[CloudDocumentOpen]] and [[MaterialDocumentOpen]], down to the three
    // outcomes: the callers are the browser's double-click, the command palette's Open group and the
    // document's own `New`, and all three need the same distinction between "that string was never one of
    // ours" and "it was one of ours and it would not resolve".
    //
    // The request carries a HANDLE and not the path, because the handle is the document's identity: it is
    // what open-or-focus is keyed on and what the window's ImGui id is built from. So the resolution has to
    // happen on this side of the wire — see Editor/Core/SubjectOpenRequest.hpp.
    enum class ShaderGraphDocumentRequest
    {
        NotAGraphPath, // the string names no `.dgraph` on disk; nothing was logged, nothing was wrong
        Failed,        // it IS a graph file and it would not resolve — logged, with the path
        Requested,     // queued; the window appears on the next frame
    };

    [[nodiscard]] inline ShaderGraphDocumentRequest
    RequestShaderGraphDocument( Assets::AssetManager* assetManager, const std::string& assetPath )
    {
        if ( !assetManager )
            return ShaderGraphDocumentRequest::NotAGraphPath;

        // Checked BEFORE anything is created: AssetManager will happily mint a record for a path with no
        // file behind it, so without this a mistyped argument would open an empty document instead of
        // producing the error that names what it could have meant.
        std::error_code ec;
        const auto      path = std::filesystem::path( assetPath );
        if ( path.extension() != Assets::Serialization::ShaderGraph::kShaderGraphExtension ||
             !std::filesystem::exists( path, ec ) )
        {
            return ShaderGraphDocumentRequest::NotAGraphPath;
        }

        auto asset = assetManager->FindByPath<Assets::ShaderGraphAsset>( assetPath );
        if ( !asset )
        {
            // First time anything asked for this file. There is no preloader for shader graphs — nothing
            // outside an open editor window ever reads one, so parsing every graph in the project at boot
            // would be work for a reader that does not exist — which makes this find-or-create the ONE
            // place a `.dgraph` becomes an asset.
            asset = assetManager->CreateAsset<Assets::ShaderGraphAsset>( Assets::AssetPriority::Medium,
                                                                         assetPath );
        }

        if ( !asset )
        {
            LOG_ERROR( "[ShaderGraph] '{}' could not be opened — no editor window was created.", assetPath );
            return ShaderGraphDocumentRequest::Failed;
        }

        if ( !asset->IsReadyForUse() )
            asset->Load();

        if ( !asset->IsReadyForUse() )
        {
            LOG_ERROR( "[ShaderGraph] '{}' is a graph that would not load — no editor window was created. "
                       "The load error above says why.",
                       assetPath );
            return ShaderGraphDocumentRequest::Failed;
        }

        QueueShaderGraphSubjectOpen( asset->GetMetadata().Handle );
        return ShaderGraphDocumentRequest::Requested;
    }
} // namespace Desert::Editor
