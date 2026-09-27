#pragma once

// THE TITLE AN ASSET-BACKED DOCUMENT WEARS, IN ONE PLACE.
//
// Seven documents wanted the same sentence -- "the asset file's name, or the type's name when the asset
// is gone" -- and seven sources wrote it, each as `SubjectTitle` in its own anonymous namespace:
// CloudLayoutPanel, CloudModellingVolumePanel, CloudNoiseVolumePanel, CloudTypePanel,
// SkyboxViewerDocument, StaticMeshViewerDocument and TextureViewerDocument. Identical bodies differing
// only in the fallback string, and in whether the lookup is typed.
//
// WHAT MADE THAT COST SOMETHING. An anonymous namespace is per translation unit, so the copies were
// invisible until the Windows CI job started compiling Editor as MSBuild unity files
// (BuildScripts/UnityBuild.lua): two of them landing in one group is a redefinition. Three copies were
// already carried on that file's opt-out list for exactly this, and the fourth failed the first Windows
// run with --unity (36260237438, StaticMeshViewerDocument.cpp(27) C2084). Opting each one out as it
// surfaces is a list that grows with the grouping, and the grouping moves every time the list does --
// so the copies go instead.
//
// NOT IN EditorSubject.hpp, which is where `AssetSubject` lives and would otherwise be the obvious home:
// that header is deliberately free of the engine (its own top note says what IPanel.hpp drags along), and
// a title needs the AssetManager. This one is included by the documents that draw a title and by nothing
// else.

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/ContentRegistry.hpp>

#include <string>
#include <string_view>

namespace Desert::Editor
{
    // BY METADATA. The document names the file whatever the asset turns out to be -- what the three
    // viewers want, since a viewer is opened on a handle the asset browser already resolved.
    //
    // Falls back to @p fallback rather than to something empty: a document whose asset has gone missing
    // still has to be a window the user can find and close.
    [[nodiscard]] inline std::string AssetSubjectTitle( const Assets::AssetHandle& subject,
                                                        Assets::AssetManager*      assets,
                                                        const std::string_view     fallback )
    {
        if ( assets != nullptr )
        {
            if ( const auto* meta = assets->FindMetadataByHandle( subject ) )
                return meta->Filepath.filename().string();
        }
        return std::string( fallback );
    }

    // BY TYPE. The title appears only when the subject really is a @p TAsset; anything else reads as the
    // fallback. That is the cloud panels' rule and not the viewers': a cloud document is opened from a
    // chain of handles that may name a slot nothing has been assigned to yet, and printing a .detex's
    // filename on a Cloud Type window would be a wrong answer where "Cloud Type" is an honest one.
    template <typename TAsset>
    [[nodiscard]] std::string AssetSubjectTitle( const Assets::AssetHandle& subject, Assets::AssetManager* assets,
                                                 const std::string_view fallback )
    {
        if ( assets != nullptr )
        {
            if ( const auto asset = assets->FindByHandle<TAsset>( subject ) )
                return asset->GetMetadata().Filepath.filename().string();
        }
        return std::string( fallback );
    }

    // BY TYPE, FOR A KIND CREATED ON DEMAND. The document is constructed before its asset exists: the
    // title is baked into ISubjectDocument's constructor, and the constructor is what creates the shell. So
    // the lookup PROBES (a miss is the expected case, not a defect) and a miss reads the registry row of
    // @p kind, which names the file without creating anything.
    template <typename TAsset>
    [[nodiscard]] std::string AssetSubjectTitle( const Assets::AssetHandle& subject, Assets::AssetManager* assets,
                                                 const Common::Content::ContentKind kind,
                                                 const std::string_view             fallback )
    {
        if ( assets != nullptr )
        {
            if ( const auto asset = assets->ProbeByHandle<TAsset>( subject ) )
                return asset->GetMetadata().Filepath.filename().string();
        }
        if ( const auto row = Assets::ContentRegistry::RowOf( kind, static_cast<uint64_t>( subject ) ) )
            return row->Path.filename().string();
        return std::string( fallback );
    }
} // namespace Desert::Editor
