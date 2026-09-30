#pragma once

#include <Common/Content/ContentKinds.hpp>

#include <optional>
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
        ImportSettings,

        // THE ANIMATION ASSETS (THM1n-4). Each is its own kind because each has its own class colour, icon,
        // filter entry and — for the first three — its own picture (UE: USkeletalMesh, USkeleton,
        // UAnimSequence thumbnail renderers).
        SkinnedMesh, // `.skmesh`: photographed in its bind pose
        Skeleton,    // `.skeleton`: its preview mesh photographed in the bind pose
        Animation,   // `.anim`: its skeleton's preview mesh photographed at the clip's middle
        ControlRig,  // `.derig`: a rig document
        AnimGraph,   // `.danimgraph`: UE's AnimBlueprint
        Retarget,    // `.retarget`: a retarget document

        FoliageType, // `.defoliage`: UE's UFoliageType
        StringTable, // `.destrings`: a localisation table

        /// A partitioned world's cooked cells and index (`.dwcell`, `.dwindex`). ONE kind for two extensions
        /// for the reason Cloud is: one colour, one icon, one producer, and neither is authored — both are
        /// written by the streaming cook beside their scene.
        CookedWorld,

        /// A SKYBOX (a panorama `.detex` under the Skybox root; UE: a TextureCube). It shares its extension
        /// with Texture and is told apart by its ROOT (Common ContentScan's KindOfContentFile), so it is in
        /// no row of kFileExtensions: FileTypeOfContent types it. Its own kind because its picture is its own
        /// — the sky drawn under the dome camera (ThumbnailProducers::Producer::RenderedSky), not the decoded
        /// strip a texture shows.
        Skybox
    };

    /// The last enumerator: the censuses that walk the enum (ThumbnailProducers) stop here, so adding a kind
    /// is one edit of this line rather than a bound hidden in each suite.
    inline constexpr FileType kLastFileType = FileType::Skybox;

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
         { "skmesh", FileType::SkinnedMesh },
         { "skeleton", FileType::Skeleton },
         { "anim", FileType::Animation },
         { "derig", FileType::ControlRig },
         { "danimgraph", FileType::AnimGraph },
         { "retarget", FileType::Retarget },
         { "defoliage", FileType::FoliageType },
         { "destrings", FileType::StringTable },
         { "dwcell", FileType::CookedWorld },
         { "dwindex", FileType::CookedWorld },
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

    /// The kind of a CONTENT file: @p extension as FileTypeOf types it, except where the content kind the
    /// file's root gives (Common::Content::KindOfContentFile) is one the extension cannot say — a Skybox
    /// `.detex` is not a Texture `.detex`. @p kind is nullopt for a file no content scan enumerates.
    [[nodiscard]] constexpr FileType FileTypeOfContent( std::string_view                                  extension,
                                                        std::optional<Common::Content::ContentKind> kind ) noexcept
    {
        if ( kind == Common::Content::ContentKind::Skybox )
            return FileType::Skybox;
        return FileTypeOf( extension );
    }
} // namespace Desert::Editor
