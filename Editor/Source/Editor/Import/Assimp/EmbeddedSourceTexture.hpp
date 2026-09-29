#pragma once

#include <Editor/Import/TextureChannelPack.hpp>

#include <filesystem>
#include <optional>
#include <string>

struct aiScene;
struct aiTexture;

namespace Desert::Editor
{
    // Where a source material's texture reference lives, once it is on disk: `Extracted` is set when the
    // reference named an image EMBEDDED in the source (a .glb's bufferView image, an FBX with embedded media -
    // assimp's "*N" or a name matching one of scene.mTextures) and says whether this import wrote it.
    // An empty Path is "not found", as FindSourceTexture reports it.
    struct SourceTextureFile
    {
        std::filesystem::path      Path;
        std::optional<PackOutcome> Extracted;
    };

    // The derived file an embedded texture is written to: beside the source, `<source stem>_<index>.<ext>`, so a
    // re-import names the same file and the texture importer keeps its GUID. `ext` is the compressed image's own
    // format (the source's bytes are kept as they are); an uncompressed texel array is encoded to PNG.
    std::filesystem::path EmbeddedTexturePath( const std::filesystem::path& sourcePath, unsigned index,
                                               const aiTexture& texture );

    // Resolves a reference of the material read from `scene` (imported from `sourcePath`): an embedded texture is
    // written by WriteDerivedTexture (only when its bytes changed) and its derived path returned; any other
    // reference is looked up on disk beside the source (FindSourceTexture). Refuses an embedded texture it cannot
    // encode or write, or one in a format no texture importer reads, naming it.
    Common::ResultStr<SourceTextureFile> ResolveSourceTexture( const aiScene&               scene,
                                                               const std::filesystem::path& sourcePath,
                                                               const std::string&           reference );
} // namespace Desert::Editor
