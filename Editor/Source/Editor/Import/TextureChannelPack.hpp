#pragma once

#include <Editor/Import/MaterialImportContract.hpp>

#include <filesystem>
#include <string_view>

namespace Desert::Editor
{
    // THE DERIVED IMAGE OF A PACKED SLOT (MAT1b, lead decision B): a template slot fed by more than one source
    // image, or by one image that does not fill every channel the template routes there, gets one image built
    // at import, straight into its TEXTURE ASSET (`.detex`; no intermediate image in the content, as UE
    // Interchange creates a UTexture2D). Its path is a function of the sources alone (beside the first, named by
    // every source's stem and the slot), so a re-import finds the same asset and keeps its GUID.
    std::filesystem::path PackedTexturePath( const ImportedTextureSlot& slot );

    // Whether a derived texture touched its asset. A packed or embedded image is an IMPORTED texture asset (as
    // UE Interchange creates textures in the content), so a re-import with unchanged inputs must leave it alone:
    // its bytes, its mtime and its GUID all stay as they were; changed inputs re-take the source, GUID kept.
    enum class PackOutcome
    {
        Written,  // no asset yet, or the inputs changed the source it carries
        Unchanged // the asset already carries exactly these bytes; the file was not rewritten
    };

    // Packs the slot's parts into one RGBA PNG carried by the texture asset `out` (WriteDerivedTexture): each
    // part's channels land in the same channels of the output (`.gb` -> G and B), a channel no part fills is
    // white (the factor passes through unchanged). A part is a loose image or a texture asset (an embedded
    // texture's), read through Assets::ReadTextureSourceImage. The asset's source hash IS the record of the
    // inputs it was built from - no sidecar to go stale beside it. Refuses an unreadable source or sources of
    // different sizes, naming the files.
    Common::ResultStr<PackOutcome> PackTextureChannels( const ImportedTextureSlot&   slot,
                                                        const std::filesystem::path& out );

    // The one rule every image DERIVED at import follows (a packed slot, an embedded texture pulled out of a .glb
    // or .fbx): the encoded image `bytes` (an `imageExtension` file: ".png", ".jpg" ...) become the source of the
    // texture asset `asset` (a `.detex`, project key or absolute) through Assets::WriteTextureSource — created
    // with a fresh GUID, or re-taken with its GUID kept, or left untouched when it already carries them. No image
    // file is written into the content: the asset carries its source, as UE's UTexture::Source does.
    Common::ResultStr<PackOutcome> WriteDerivedTexture( std::string_view bytes, const std::filesystem::path& asset,
                                                        std::string_view imageExtension );
} // namespace Desert::Editor
