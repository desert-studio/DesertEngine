#pragma once

// DELIBERATELY NOT <Editor/Core/SubjectOpenRequest.hpp>, even though queueing one is the whole point of this
// header. That header opens `namespace Desert::Editor::Core`, and this one is included by the four cloud
// panels — which spell Desert::Core::Formats as an unqualified `Core::Formats` from inside Desert::Editor.
// Make Desert::Editor::Core visible before those uses and every one of them silently rebinds to the wrong
// namespace; it does not silently compile, but the error names a namespace nobody wrote.
// SubjectEditorRegistry.hpp carries the same note and escapes the same way. So the queueing is declared here and
// DEFINED in CloudDocumentOpen.cpp, and nothing in this header drags that namespace along.

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/CloudLayout.hpp>
#include <Engine/Assets/CloudLayoutAsset.hpp>
#include <Engine/Assets/CloudModellingVolume.hpp>
#include <Engine/Assets/CloudModellingVolumeAsset.hpp>
#include <Engine/Assets/CloudNoiseVolume.hpp>
#include <Engine/Assets/CloudNoiseVolumeAsset.hpp>
#include <Engine/Assets/CloudTypeAsset.hpp>
#include <Engine/Assets/CloudTypeData.hpp>

#include <Common/Core/Logger.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace Desert::Editor
{
    // Hands a resolved subject to Core::SubjectOpenRequests. Declared here and defined in the matching .cpp
    // for the namespace reason at the top of this file — the handle and the type rather than the request
    // itself, so this header never sees Desert::Editor::Core.
    void QueueCloudSubjectOpen( const Assets::AssetHandle& subject, Assets::AssetTypeID type );

    // "Open the cloud asset at this PATH in whatever edits it."
    //
    // The cloud half of [[MaterialDocumentOpen]], and deliberately its twin down to the three outcomes: the
    // two callers are the same two (the browser's double-click, and EditorLayer's boot-time `--open-panel`
    // resolution), and they need the same distinction between "that string was never one of ours" and "it
    // was one of ours and it would not resolve".
    //
    // ONE HEADER FOR FOUR FORMATS rather than four copies of the material one. The resolution is identical
    // for all four — check the extension, find-or-create, load, resolve dependencies, queue the handle — and
    // the only things that vary are the asset class and its extension constant, which is exactly what a
    // template parameter is. Four hand-written copies is how three of them come to lack the
    // ResolveDependencies call and the fourth opens a type whose noise volume is null.
    //
    // The request carries a HANDLE and not the path, because the handle is the document's identity: it is
    // what open-or-focus is keyed on and what the window's ImGui id is built from. So the resolution has to
    // happen on this side of the wire — see Editor/Core/SubjectOpenRequest.hpp.
    enum class CloudDocumentRequest
    {
        NotACloudPath, // the string names no cloud asset on disk; nothing was logged, nothing was wrong
        Failed,        // it IS a cloud file and it would not resolve — logged, with the path
        Requested,     // queued; the window appears on the next frame
    };

    // The resolution for ONE cloud format. @p extension is the format's own constant (kCloudTypeExtension
    // and friends), taken as a parameter rather than read off the asset class because the asset classes do
    // not carry it — the constant lives beside the DATA type, not beside the asset wrapper.
    template <typename AssetT>
    inline CloudDocumentRequest RequestCloudDocumentOfType( Assets::AssetManager* assetManager,
                                                            const std::string&    assetPath,
                                                            std::string_view      extension )
    {
        if ( !assetManager )
            return CloudDocumentRequest::NotACloudPath;

        // Checked BEFORE anything is created: AssetManager will happily mint a record for a path with no
        // file behind it, so without this a mistyped argument would open an empty document instead of
        // producing the error that names what it could have meant. Same order, and for the same reason, as
        // RequestMaterialDocument.
        std::error_code ec;
        const auto      path = std::filesystem::path( assetPath );
        if ( path.extension() != extension || !std::filesystem::exists( path, ec ) )
            return CloudDocumentRequest::NotACloudPath;

        auto asset = assetManager->FindByPath<AssetT>( assetPath );
        if ( !asset )
        {
            // First time anything asked for this file. Noise volumes, hero-cloud bodies and painted layouts
            // have no boot stage (AL1-2): their shells exist only once a scene or a window has named them,
            // so this is the ordinary path from the Content Browser, not an exception to it.
            asset = assetManager->CreateAsset<AssetT>( assetPath );
        }

        if ( !asset )
        {
            LOG_ERROR( "[Assets] '{}' could not be opened as a cloud asset — no editor window was created.",
                       assetPath );
            return CloudDocumentRequest::Failed;
        }

        if ( !asset->IsReadyForUse() )
            asset->Load();

        // A cloud TYPE names the noise volume its edge is cut from, and that binding is what
        // ResolveDependencies does. Called for every format through the base's virtual — the three that
        // have no dependencies inherit a no-op, which is cheaper than four call sites that have to remember
        // which of them needs it.
        asset->ResolveDependencies( *assetManager );

        if ( !asset->IsReadyForUse() )
        {
            LOG_ERROR( "[Assets] '{}' is a cloud asset that would not load — no editor window was created. "
                       "The load error above says why.",
                       assetPath );
            return CloudDocumentRequest::Failed;
        }

        QueueCloudSubjectOpen( asset->GetMetadata().Handle, AssetT::GetTypeID() );
        return CloudDocumentRequest::Requested;
    }

    // Any of the four cloud formats, dispatched on the extension.
    //
    // The order is not significant — the four extensions are disjoint — but the CHAIN is: a caller that
    // holds a path and does not care which kind of cloud asset it is gets one call, and a fifth format
    // added later is one more line here rather than one more branch at each call site.
    inline CloudDocumentRequest RequestCloudDocument( Assets::AssetManager* assetManager,
                                                      const std::string&    assetPath )
    {
        using Assets::CloudLayoutAsset;
        using Assets::CloudModellingVolumeAsset;
        using Assets::CloudNoiseVolumeAsset;
        using Assets::CloudTypeAsset;

        if ( const auto noise = RequestCloudDocumentOfType<CloudNoiseVolumeAsset>(
                  assetManager, assetPath, Assets::kCloudNoiseVolumeExtension );
             noise != CloudDocumentRequest::NotACloudPath )
        {
            return noise;
        }

        if ( const auto type = RequestCloudDocumentOfType<CloudTypeAsset>( assetManager, assetPath,
                                                                           Assets::kCloudTypeExtension );
             type != CloudDocumentRequest::NotACloudPath )
        {
            return type;
        }

        if ( const auto body = RequestCloudDocumentOfType<CloudModellingVolumeAsset>(
                  assetManager, assetPath, Assets::kCloudModellingVolumeExtension );
             body != CloudDocumentRequest::NotACloudPath )
        {
            return body;
        }

        return RequestCloudDocumentOfType<CloudLayoutAsset>( assetManager, assetPath,
                                                             Assets::kCloudLayoutExtension );
    }

    // Does @p assetPath end in one of the four cloud extensions? Asked by the asset browser, which has to
    // decide whether a double-click is ITS business before it has an AssetManager answer — and which must
    // report a cloud file that failed to open rather than letting the click do nothing silently.
    [[nodiscard]] inline bool IsCloudAssetPath( const std::string& assetPath )
    {
        const auto extension = std::filesystem::path( assetPath ).extension();
        return extension == Assets::kCloudNoiseVolumeExtension || extension == Assets::kCloudTypeExtension ||
               extension == Assets::kCloudModellingVolumeExtension || extension == Assets::kCloudLayoutExtension;
    }
} // namespace Desert::Editor
