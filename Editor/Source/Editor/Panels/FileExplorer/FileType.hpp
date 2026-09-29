#pragma once

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
        LandscapeLayerInfo
    };
} // namespace Desert::Editor
