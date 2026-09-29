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

    // Writes the slot's parts into one RGBA PNG at `out`: each part's channels land in the same channels of the
    // output (`.gb` -> G and B), a channel no part fills is white (the factor passes through unchanged). Refuses
    // an unreadable source or sources of different sizes, naming the files.
    Common::BoolResultStr PackTextureChannels( const ImportedTextureSlot& slot, const std::filesystem::path& out );
} // namespace Desert::Editor
