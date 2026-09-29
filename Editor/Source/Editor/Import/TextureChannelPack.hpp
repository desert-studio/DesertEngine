#pragma once

#include <Editor/Import/MaterialImportContract.hpp>

#include <filesystem>

namespace Desert::Editor
{
    // THE DERIVED IMAGE OF A PACKED SLOT (MAT1b, lead decision B): a template slot fed by more than one source
    // image, or by one image that does not fill every channel the template routes there, gets one image built
    // at import. Its path is a function of the sources alone (beside the first, named by every source's stem
    // and the slot), so a re-import writes the same file and the texture importer keeps its GUID.
    std::filesystem::path PackedTexturePath( const ImportedTextureSlot& slot );

    // Whether a pack touched the file. A packed image is an IMPORTED texture asset (as UE Interchange creates
    // textures in the content), so a re-import with unchanged inputs must leave it alone: its bytes, its mtime
    // and so the texture importer's work and the asset's GUID (kept by path) all stay as they were.
    enum class PackOutcome
    {
        Written,  // no file yet, or the inputs changed what it holds
        Unchanged // the inputs pack to exactly the bytes already there; the file was not rewritten
    };

    // Packs the slot's parts into one RGBA PNG at `out`: each part's channels land in the same channels of the
    // output (`.gb` -> G and B), a channel no part fills is white (the factor passes through unchanged). The
    // file on disk IS the record of the inputs it was built from - no sidecar hash to go stale beside it: the
    // pack is encoded in memory and written only when its bytes differ from the file's. Refuses an unreadable
    // source or sources of different sizes, naming the files.
    Common::ResultStr<PackOutcome> PackTextureChannels( const ImportedTextureSlot&   slot,
                                                        const std::filesystem::path& out );
} // namespace Desert::Editor
