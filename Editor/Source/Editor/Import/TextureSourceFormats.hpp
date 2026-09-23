#pragma once

// The ONE ordered list of texture source formats the import pipeline recognizes.
//
// Two hand-written copies of this priority used to exist — AssimpImporter's extension fallback and
// FbxMeshSplitter's FormatRank — and they had already drifted: the importer ranked `.jpg` ABOVE `.tga`,
// so a lossy JPEG sitting next to a model beat the lossless TGA of the same stem, silently baking
// compression artifacts into normal maps and masks. The splitter disagreed and preferred the TGA. Both
// looked locally reasonable; only the relation between them was wrong. The list now lives here alone,
// and a census test (Desert/Tests/Editor/TextureSourceFormatCensus) fails the build of anyone who
// starts a second one.
//
// The ORDER is the statement. When several files share a stem and only the extension differs, the
// earliest entry wins, so:
//
//   1. Lossless LDR first: `.tga`, `.png`, `.bmp`. A lossy sibling of a lossless file is almost always
//      a derivative (a preview, a web export); picking it loses data with no error anywhere. TGA before
//      PNG is the project owner's call: TGA is the DCC working/export format here, so when both ship
//      together the TGA is the original and the PNG the conversion. BMP closes the group — lossless,
//      but with unreliable alpha and near-certain to be a conversion artifact when a sibling exists.
//   2. Lossy after: `.jpg`, `.jpeg`. Acceptable when they are all that ships (photo-scanned albedo
//      often is), never preferred over a lossless twin.
//   3. Extended-range last, as its own group: `.exr`, `.hdr`. These are float-range DATA, not a drop-in
//      LDR replacement — the cook keeps them RGBA32F and uncompressed (16 bytes a texel, and no block
//      format is offered to a float source), so for a material slot an LDR sibling is always the better
//      pick. They are still listed so that a stem shipping ONLY as `.exr`/`.hdr` is found, and so that the
//      sky panorama in `Assets/Textures/HDR/` is cooked like any other texture — the runtime reads its
//      `.tex` and holds no decoder (`Engine/Graphic/Environment/SceneEnvironment.cpp`). EXR fails the
//      cook loudly (stb cannot decode it) instead of silently leaving the slot empty.
//
// Reorder this list only with a reason written here; the previous order was "whatever each copy's
// author typed", and that is how the JPEG-beats-TGA defect shipped.

#include <cstddef>
#include <string_view>

namespace Desert::Editor
{
    inline constexpr const char* kTextureSourceExtensions[] = {
         ".tga", ".png", ".bmp", ".jpg", ".jpeg", ".exr", ".hdr",
    };

    inline constexpr std::size_t kTextureSourceExtensionCount =
         sizeof( kTextureSourceExtensions ) / sizeof( kTextureSourceExtensions[0] );

    // Rank of an extension in the priority order: 0 is the most preferred; kTextureSourceExtensionCount
    // means "not an image source we use". `extLower` must already be lower-case and dot-prefixed.
    constexpr std::size_t TextureSourceFormatRank( std::string_view extLower )
    {
        for ( std::size_t i = 0; i < kTextureSourceExtensionCount; ++i )
        {
            if ( extLower == kTextureSourceExtensions[i] )
                return i;
        }
        return kTextureSourceExtensionCount;
    }
} // namespace Desert::Editor
