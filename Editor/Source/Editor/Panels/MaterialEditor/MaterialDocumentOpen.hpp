#pragma once

#include <Editor/Core/AssetOpen.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/Mesh/SurfaceMaterialAsset.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>

#include <filesystem>
#include <string>
#include <system_error>

namespace Desert::Editor
{
    // "Open the Material Editor on the `.demat` at this PATH."
    //
    // One implementation for the two callers that have a path rather than a handle — the asset browser's
    // double-click, and EditorLayer's boot-time --open-panel resolution. Written once because resolving a
    // material file to a REGISTERED asset has three steps that are easy to get subtly different (find,
    // create-and-load if this is the first ask, register the shell), and two copies of it would be the
    // two-implementations-of-one-quantity shape this engine keeps paying for.
    //
    // The request carries a SUBJECT and not the path, because the subject is the document's identity: it is
    // what open-or-focus is keyed on and what the window's ImGui id is built from. So the resolution has to
    // happen on this side of the wire.
    //
    // The three outcomes are kept apart because the two callers need different ones. EditorLayer's
    // --open-panel is GUESSING at what a string means, so it has to be able to try and move on — but only
    // when the string was never a material to begin with. A `.demat` that failed to resolve has already been
    // reported here, and a caller that then reported it again in its own words would print two messages, the
    // second of them wrong.
    enum class MaterialDocumentRequest
    {
        NotAMaterialPath, // the string names no `.demat` on disk; nothing was logged, nothing was wrong
        Failed,           // it IS a material file and it would not resolve — logged, with the path
        Requested,        // queued; the window appears on the next frame
    };

    // WHAT THE MATERIAL EDITOR NEEDS BEFORE IT BINDS: the asset LOADED. Called by the editor's registration
    // factory (EditorLayer.cpp), so every route that reaches the window — a browser double-click, a Details
    // pencil, a field's "Open", a palette command — opens a material whose asset was only a record as well
    // as a loaded one; before AV1c only the by-PATH route loaded, and a by-handle open bound an empty asset.
    [[nodiscard]] inline Common::ResultStr<Assets::Asset<Assets::SurfaceMaterialAsset>>
    EnsureMaterialLoaded( Assets::AssetManager& assetManager, const Assets::AssetHandle& handle )
    {
        auto asset = assetManager.FindByHandle<Assets::SurfaceMaterialAsset>( handle );
        if ( !asset )
            return Common::MakeFormattedError<Assets::Asset<Assets::SurfaceMaterialAsset>>(
                 "material {:016x} is not in the asset database", static_cast<uint64_t>( handle ) );
        if ( !asset->IsReadyForUse() )
        {
            const auto loaded = asset->Load();
            if ( !loaded.IsSuccess() )
                return Common::MakeFormattedError<Assets::Asset<Assets::SurfaceMaterialAsset>>(
                     "material {:016x} ('{}') did not load: {}", static_cast<uint64_t>( handle ),
                     asset->GetMetadata().Filepath.generic_string(), loaded.GetError() );
        }
        return Common::MakeSuccess( asset );
    }

    // A `.demat` PATH (browser double-click, --open-panel, the palette's Open) to the by-handle route. Only the
    // record is made here; loading is EnsureMaterialLoaded's, in the registration factory.
    inline MaterialDocumentRequest RequestMaterialDocument( Assets::AssetManager*        assetManager,
                                                            const std::string&           assetPath,
                                                            const SubjectEditorRegistry& editors )
    {
        if ( !assetManager )
            return MaterialDocumentRequest::NotAMaterialPath;

        // Checked BEFORE anything is created: AssetManager will happily mint a record for a path with no
        // file behind it, so without this a mistyped argument would open an empty document instead of
        // producing the error that names what it could have meant.
        std::error_code ec;
        const auto      path = std::filesystem::path( assetPath );
        if ( path.extension() != Common::Constants::Extensions::MATERIAL_EXTENSION ||
             !std::filesystem::exists( path, ec ) )
        {
            return MaterialDocumentRequest::NotAMaterialPath;
        }

        auto asset = assetManager->FindByPath<Assets::SurfaceMaterialAsset>( assetPath );
        if ( !asset )
            asset =
                 assetManager->CreateAsset<Assets::SurfaceMaterialAsset>( Assets::AssetPriority::High, assetPath,
                                                                          /*loadAfterCreate=*/false );
        if ( !asset )
        {
            LOG_ERROR( "[Assets] '{}' could not be opened as a material — no Material Editor window was "
                       "created.",
                       assetPath );
            return MaterialDocumentRequest::Failed;
        }

        const auto handle = asset->GetMetadata().Handle;
        const auto opened =
             Core::RequestOpenAsset( assetManager->FindMetadataByHandle( handle ), handle, editors );
        if ( !opened.IsSuccess() )
        {
            LOG_ERROR( "[Assets] '{}': {}", assetPath, opened.GetError() );
            return MaterialDocumentRequest::Failed;
        }
        return MaterialDocumentRequest::Requested;
    }
} // namespace Desert::Editor
