#pragma once

#include <string_view>

// The browser's asset kinds, on their own so a device-free header (Widgets/ThumbnailProducers.hpp) and its
// suite can name them without the panel's ImGui closure.
namespace Desert::Editor
{
    enum class FileType
    {
        Unknown = 0,
        Scene,
        Prefab,
        Script,
        Audio,
        Shader,
        Texture,
        Cubemap,
        Model,
        Material,
        ShaderGraph,
        // `Project` USED TO SIT HERE AND WAS DEAD IN BOTH DIRECTIONS: no extension mapped to it and no
        // code read it. It could not have worked either — a `.deproj` lives at the PROJECT root, above
        // the assets root this browser is rooted at, so the tile it typed can never be drawn.
        Ini,
        Font,

        /// The four cloud formats — `.dclayout`, `.dcnv`, `.dcmv`, `.decloudtype`.
        ///
        /// ONE TYPE FOR FOUR EXTENSIONS, and the alternative was four. They share a colour, an icon, a
        /// filter entry and — the reason that decides it — a THUMBNAIL PRODUCER: all four are painted
        /// from their own bytes by Editor/Widgets/CloudThumbnail.hpp, so every branch that would
        /// distinguish them here would immediately re-join. What tells them apart is the document each
        /// one opens, and that is the subject-editor registry's question, not this enum's.
        ///
        /// THEY WERE `Unknown` UNTIL M11, which is why the owner could not pick a cloud by looking: an
        /// unknown type gets the generic document glyph, so four different assets drew one identical
        /// grey square and the browser's own type filter could not name them.
        Cloud,

        /// A UI theme (`.detheme`) — named colours, metrics and fonts plus the styles that bind them.
        /// Its OWN type rather than sharing one: it has no producer in common with anything above (a
        /// theme is not painted from bytes the way a cloud is), and the browser's type filter has to be
        /// able to name it, which is the whole reason the cloud formats stopped being `Unknown`.
        UITheme,

        /// A landscape layer info (`.delayerinfo`, UE ULandscapeLayerInfoObject): its own type so the
        /// browser can colour it, give it an icon and filter by it; it has no producer in common with any
        /// type above.
        LandscapeLayerInfo,

        /// An import settings sidecar (`.deimport`) written beside a source file by the importer: it states
        /// HOW the source is brought in, so the browser names it instead of calling it Unknown.
        ImportSettings
    };

    /// ONE MAP: EXTENSION -> KIND (THM1n-3). The link before ThumbnailProducers in the chain
    /// "extension -> FileType -> producer" — the Content Browser types a file here and nowhere else, and the
    /// ThumbnailFormats suite holds every engine asset extension (Common ContentKinds) against it, so an
    /// asset format the browser does not know is a red census rather than a silent grey glyph.
    /// Lower case, WITHOUT the leading dot.
    struct FileExtension
    {
        std::string_view Extension;
        FileType         Type;
    };

    inline constexpr FileExtension kFileExtensions[] = {
         { "lsn", FileType::Scene },
         { "desce", FileType::Scene },
         { "deprefab", FileType::Prefab },
         { "prefab", FileType::Prefab },
         { "lprefab", FileType::Prefab },
         { "cs", FileType::Script },
         { "lua", FileType::Script },
         { "glsl", FileType::Shader },
         { "shader", FileType::Shader },
         { "frag", FileType::Shader },
         { "vert", FileType::Shader },
         { "comp", FileType::Shader },
         { "png", FileType::Texture },
         { "jpg", FileType::Texture },
         { "jpeg", FileType::Texture },
         { "bmp", FileType::Texture },
         { "gif", FileType::Texture },
         { "tga", FileType::Texture },
         // The imported texture ASSET (an envelope whose SRCE section is the source file's bytes verbatim).
         // It was in no map until THM1n-3, so the Textures folder showed a grey glyph per texture.
         { "detex", FileType::Texture },
         { "ttf", FileType::Font },
         { "hdr", FileType::Cubemap },
         { "obj", FileType::Model },
         { "fbx", FileType::Model },
         { "gltf", FileType::Model },
         { "glb", FileType::Model },
         { "blend", FileType::Model },
         { "demesh", FileType::Model },
         // A split import's node meshes (THM1k): `.stmesh` files with no source beside them.
         { "stmesh", FileType::Model },
         { "mp3", FileType::Audio },
         { "m4a", FileType::Audio },
         { "wav", FileType::Audio },
         { "ogg", FileType::Audio },
         { "demat", FileType::Material },
         { "lmat", FileType::Material },
         { "dgraph", FileType::ShaderGraph },
         { "ini", FileType::Ini },
         { "dclayout", FileType::Cloud },
         { "dcnv", FileType::Cloud },
         { "dcmv", FileType::Cloud },
         { "decloudtype", FileType::Cloud },
         { "detheme", FileType::UITheme },
         { "delayerinfo", FileType::LandscapeLayerInfo },
         { "deimport", FileType::ImportSettings },
    };

    /// The kind of a file with @p extension (lower case, no dot); Unknown for one the map does not name.
    [[nodiscard]] constexpr FileType FileTypeOf( std::string_view extension ) noexcept
    {
        for ( const FileExtension& row : kFileExtensions )
        {
            if ( row.Extension == extension )
                return row.Type;
        }
        return FileType::Unknown;
    }
} // namespace Desert::Editor
