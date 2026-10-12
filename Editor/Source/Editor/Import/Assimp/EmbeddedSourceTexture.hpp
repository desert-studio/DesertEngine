#pragma once

#include <Editor/Import/IAssetImporter.hpp>
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
    // assimp's "*N" or a name matching one of scene.mTextures) and says whether this import wrote its texture
    // asset; `Path` is then that asset (`.detex`), never an image file.
    // An empty Path is "not found", as FindSourceTexture reports it.
    struct SourceTextureFile
    {
        std::filesystem::path      Path;
        std::optional<PackOutcome> Extracted;
    };

    // The TEXTURE ASSET an embedded texture is imported into: beside the source, `<source stem>_<index>.detex`,
    // so a re-import finds the same asset and keeps its GUID (UE Interchange: the embedded image becomes its own
    // UTexture2D in the import folder). No image file is written beside it - the asset carries the bytes.
    std::filesystem::path EmbeddedTexturePath( const std::filesystem::path& sourcePath, unsigned index );

    // The image format the embedded texture's bytes are in, as an extension (".png", ".jpg" ...): a compressed
    // texture's own (its bytes are kept as they are), PNG for an uncompressed texel array (encoded at import).
    std::string EmbeddedTextureExtension( const aiTexture& texture );

    // Resolves a reference of the material read from `scene` (imported from `sourcePath`); any reference that is
    // not embedded is looked up on disk beside the source (FindSourceTexture). An embedded texture's asset is
    // CONTENT, so only `ImportPass::Import` writes it (WriteDerivedTexture: re-taken only when its bytes changed,
    // GUID kept). `ImportPass::Cook` writes nothing into Content: it returns the asset the import left
    // (Extracted = Unchanged) and refuses, naming the asset, when the source was never imported with it - the
    // cure is a re-import, not a file the editor drops into the checkout on start (SELF-COOK). Also refuses an
    // embedded texture it cannot encode or write, or one in a format no texture importer reads, naming it.
    Common::ResultStr<SourceTextureFile> ResolveSourceTexture( const aiScene&               scene,
                                                               const std::filesystem::path& sourcePath,
                                                               const std::string& reference, ImportPass pass );
} // namespace Desert::Editor
