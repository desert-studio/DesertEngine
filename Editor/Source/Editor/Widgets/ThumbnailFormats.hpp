#pragma once

#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

namespace Desert::Editor::ThumbnailFormats
{
    /**
     * @file
     * @brief EVERY FILE THE CONTENT BROWSER CAN SHOW, AND WHO MAKES ITS PICTURE.
     *
     * WHY THIS EXISTS AS A TABLE AND NOT AS THE CHAIN OF `if`s IT REPLACES. The editor's answer to "what
     * does this asset look like?" was three `else if`s in one panel function, and the measured consequence
     * was that `ThumbnailService` knew exactly TWO formats — Material and Mesh. The four cloud formats
     * (`.dclayout`, `.dcnv`, `.dcmv`, `.decloudtype`) had no producer at all, so the one place an artist
     * picks a cloud species showed four identical grey document glyphs. Nothing was broken; nothing said
     * anything either. A silent grey icon is the "empty successful answer" the contract forbids
     * (DEV_CONTRACT §1.4), wearing an icon instead of a return value.
     *
     * So the SET is written down, and it is the set the browser can show rather than the set somebody
     * remembered. Each row is one of two statements:
     *
     *   * a PRODUCER — this is who makes the picture and what the picture is OF;
     *   * `Producer::None` with a NAMED REASON why there must not be one.
     *
     * "Not built yet" is not a reason and must never appear here: that is a TODO wearing a table row
     * (DEV_CONTRACT §1.1). A `None` row states why a picture would be the WRONG answer for that format.
     *
     * THE ROW IS A CLAIM AND THE CLAIM IS CHECKED. `Desert/Tests/Editor/ThumbnailFormats` derives the
     * browser's extension set from `FileExplorerPanel.cpp`'s own `s_FileTypes` table and fails on any
     * extension with no row here, and on any row here naming an extension the browser does not know. A
     * seventh format is therefore a RED TEST rather than a quiet grey icon — which is the whole point,
     * because the six that came before it each arrived without anyone noticing this question existed.
     *
     * IT IS FREE OF THE DEVICE AND OF ImGui ON PURPOSE, exactly as ThumbnailKey.hpp and
     * ThumbnailFreshness.hpp next door are: the decision then belongs to a test instead of to a launched
     * editor.
     */

    enum class Producer
    {
        /// THE FILE IS ALREADY THE PICTURE. `ThumbnailCache::Get` decodes it straight off disk — no
        /// capture, no queue, no cached PNG. Nothing to generate, so the automatic sweep skips these.
        Decoded,

        /// An offscreen render of the material on a sphere (or a camera-facing card for a cutout).
        /// `ThumbnailService::RequestMaterial`. Costs a renderer slot while it runs.
        RenderedMaterial,

        /// An offscreen render of the mesh, framed by its own bounds. `ThumbnailService::RequestMesh`.
        /// The file PHOTOGRAPHED is the COOKED `.stmesh`, never the source — see the long note in
        /// FileExplorerPanel::DrawRenderedMeshThumbnail for why that side had to win. Costs a slot.
        RenderedMesh,

        /// A picture COMPUTED ON THE CPU from the file's own payload — Editor/Widgets/CloudThumbnail.hpp.
        /// No device, no renderer, no slot, and therefore no competition with the surfaces a person opens.
        /// This is what the four cloud formats get, and it is available to them precisely because each of
        /// them already stores its pixels (or, for a type, its silhouette) in the file.
        Painted,

        /// ONLY A HUMAN MAKES THIS ONE, from the viewport, through "Capture Thumbnail"
        /// (FileExplorerPanel::CaptureThumbnailFromViewport). The automatic sweep must never generate one:
        /// the picture of a level or a prefab is a COMPOSITION — where the camera stands, what is in
        /// frame — and an engine that picked that on its own would be guessing at authorship. A file with
        /// no captured picture keeps its type icon, which is a true statement about it.
        Authored,

        /// No picture, and @ref Format::What says why one would be the wrong answer.
        None
    };

    struct Format
    {
        /// Lower case, WITHOUT the leading dot — the same spelling `FileExplorerPanel`'s own `s_FileTypes`
        /// map is keyed on, so the two sets can be compared byte for byte instead of through a
        /// normalisation nobody owns.
        std::string_view Extension;

        Producer By;

        /// What the picture IS, or — for `None` — why there must not be one. Printed by the census when a
        /// row goes red, so a failure explains itself to somebody who has never read this file.
        std::string_view What;
    };

    // ------------------------------------------------------------------------------------------------
    // THE CENSUS
    //
    // Grouped by producer rather than alphabetically, because the grouping is the argument: everything
    // that costs a renderer slot sits together, everything that costs nothing sits together, and the
    // refusals sit together where they can be read as a set and disagreed with as a set.
    // ------------------------------------------------------------------------------------------------
    inline constexpr Format kFormats[] = {

         // ── The file is the picture ────────────────────────────────────────────────────────────────
         { "png", Producer::Decoded, "the image itself, decoded and downscaled by ThumbnailCache" },
         { "jpg", Producer::Decoded, "the image itself" },
         { "jpeg", Producer::Decoded, "the image itself" },
         { "bmp", Producer::Decoded, "the image itself" },
         { "gif", Producer::Decoded, "the first frame of the image" },
         { "tga", Producer::Decoded, "the image itself" },
         { "hdr", Producer::Decoded,
           "the equirectangular environment map itself. Not a render of the sky it makes: that would need "
           "the IBL bake and a slot, and the latitude-longitude strip is what an artist recognises a "
           "captured environment by. RE-EXAMINED 2026-09-23 against the owner's request for a sphere "
           "preview and UPHELD: the bake was measured at 252-386 ms with the device idle, and paying it "
           "per tile on a scroll is the wrong place for it. The sphere the owner asked for is the LIVE, "
           "orbitable ball in the Details Skybox section, which costs no extra renderer slot because that "
           "panel already owns a preview viewport" },

         // ── An offscreen render, and therefore a renderer slot ─────────────────────────────────────
         { "demat", Producer::RenderedMaterial,
           "the material on a sphere, or on a camera-facing card "
           "when it is a cutout (a foliage atlas garbles on a ball)" },
         { "lmat", Producer::RenderedMaterial,
           "the same picture as a .demat. The extension is Lumos-era and nothing in this engine writes "
           "one any more, but the browser still TYPES it as a material, so it still owes an answer here — "
           "a row is what makes that inheritance visible instead of implied" },

         { "obj", Producer::RenderedMesh, "the mesh framed by its own bounds, with its sidecar material" },
         { "fbx", Producer::RenderedMesh, "the mesh framed by its own bounds" },
         { "gltf", Producer::RenderedMesh, "the mesh framed by its own bounds" },
         { "glb", Producer::RenderedMesh, "the mesh framed by its own bounds" },
         { "blend", Producer::RenderedMesh, "the mesh framed by its own bounds" },
         { "demesh", Producer::RenderedMesh, "the mesh framed by its own bounds" },

         // ── Painted on the CPU from the file's own payload ─────────────────────────────────────────
         //
         // ALL FOUR CLOUD FORMATS, and none of them needs the renderer. Each already carries what the
         // picture is made of: a layout carries its painted planes, a noise volume and a modelling volume
         // carry baked voxels, and a type carries the sampled silhouette that IS its species. See
         // Editor/Widgets/CloudThumbnail.hpp for what each picture shows and why.
         { "dclayout", Producer::Painted,
           "the painted pattern's four species planes, laid out as quarters of one square and dimmed "
           "where the mask removes cloud — the placement field the bake will see, seen as it was painted" },
         { "dcnv", Producer::Painted,
           "a mid-depth slice of the noise volume, RGB from the three lowest-frequency channels — the "
           "cell structure the cloud edge is cut from" },
         { "dcmv", Producer::Painted,
           "the sculpted body seen from the side: the maximum density along the view axis through the "
           "stored voxels, which is the silhouette of the hero cloud itself" },
         { "detheme", Producer::Painted,
           "the theme's palette, in file order: the first eight colour tokens as horizontal bands on the "
           "family backdrop. A theme IS its colours, so this is not a chart about the file — it is the "
           "file. Eight because a 64-pixel tile holds about that many distinguishable bands, and because "
           "an author writes the load-bearing tokens first (a surface, then an accent)" },
         { "decloudtype", Producer::Painted,
           "the species' own silhouette, drawn from the sampled vertical profile the file stores: "
           "half-width against height, mirrored about the centre. It is the shape this type makes, not a "
           "chart about it. A PHOTOGRAPH of the species was refused with numbers — see CloudThumbnail.hpp" },

         // ── Only a human, from the viewport ────────────────────────────────────────────────────────
         { "desce", Producer::Authored,
           "a level. Its picture is a shot of it, and which shot is a decision about framing that belongs "
           "to whoever authored the level — 'Capture Thumbnail (from viewport)' is that decision made by "
           "hand. Generating one would also mean LOADING every level in the project to photograph it" },
         { "lsn", Producer::Authored, "a Lumos-era level; same answer as .desce" },
         { "deprefab", Producer::Authored,
           "a prefab is a subtree of entities, so a picture of it means instantiating it into a scene and "
           "framing the result — the same authorship question a level asks, with the same answer" },
         { "prefab", Producer::Authored, "same as .deprefab" },
         { "lprefab", Producer::Authored, "same as .deprefab" },

         // ── No picture, and why ────────────────────────────────────────────────────────────────────
         { "lua", Producer::None,
           "a script is text. The browser's preview pane already shows its opening lines, which tells a "
           "reader what it does; no 64-pixel square can" },
         { "cs", Producer::None, "same as .lua" },

         { "shader", Producer::None,
           "a shader is a PROGRAM, and a program has no appearance until something supplies its "
           "parameters. The picture of what a shader looks like is a .demat that uses it, and that row "
           "already exists — a second picture here would be one shader's arbitrary default standing in "
           "for every material made from it" },
         { "glsl", Producer::None, "a shader stage; same as .shader, and less complete" },
         { "frag", Producer::None, "a shader stage; same as .shader" },
         { "vert", Producer::None, "a shader stage; same as .shader" },
         { "comp", Producer::None, "a shader stage; same as .shader" },
         { "dgraph", Producer::None,
           "a shader graph compiles TO a .shader, so it inherits that refusal exactly. Its own useful "
           "picture is the node network, which is a document to open rather than a square to squint at" },

         { "ttf", Producer::None,
           "a typeface is identified by TEXT SET IN IT, and which text is a question about the reader's "
           "language rather than about the file. The tile is 64 px, where a glyph pair renders about 30 px "
           "tall and most faces at that size are indistinguishable; the file NAME under the tile is the "
           "face name, and it is the thing that actually tells two fonts apart. Where a real sample is "
           "worth having, the editor already sets live text in the face — the Details font row and the UI "
           "editor's font slot both do" },

         { "ini", Producer::None,
           "a settings file is text, and the browser's preview pane shows its opening lines — the same "
           "answer a script gets, for the same reason. Its own content is key-value pairs whose picture "
           "would be the text itself, rendered smaller" },

         { "wav", Producer::None,
           "a waveform. At tile size it is a grey smear that looks the same for every take of the same "
           "instrument, so it would distinguish nothing while costing a decode of the whole file. Audio "
           "is told apart by name and by being played" },
         { "mp3", Producer::None, "same as .wav" },
         { "m4a", Producer::None, "same as .wav" },
         { "ogg", Producer::None, "same as .wav" },
    };

    inline constexpr std::size_t kFormatCount = sizeof( kFormats ) / sizeof( kFormats[0] );

    /// The extension of @p path, lower case, without the dot. Empty for a file that has none.
    ///
    /// Written here rather than at each caller because "which format is this?" has to be asked the same
    /// way by the browser tile, by the background sweep and by the census, and `std::filesystem::path` is
    /// deliberately not in the signature: this header stays free of everything that would stop a test
    /// asking it about a string.
    [[nodiscard]] inline std::string ExtensionOf( std::string_view path )
    {
        const std::size_t dot = path.find_last_of( '.' );
        if ( dot == std::string_view::npos )
            return {};

        // A dot that is part of a directory name is not an extension: "Clouds/v1.2/Layout" has no
        // extension at all, and reading "2/Layout" as one would let a folder decide a file's format.
        const std::size_t slash = path.find_last_of( "/\\" );
        if ( slash != std::string_view::npos && dot < slash )
            return {};

        std::string ext( path.substr( dot + 1 ) );
        for ( char& c : ext )
            c = static_cast<char>( std::tolower( static_cast<unsigned char>( c ) ) );
        return ext;
    }

    /// The row for @p extension (lower case, no dot), or nullptr when the browser does not know it. A
    /// null answer means "not a format this editor shows", never "no picture" — those are different
    /// facts and the census exists so they cannot be confused.
    [[nodiscard]] constexpr const Format* Find( std::string_view extension ) noexcept
    {
        for ( const Format& format : kFormats )
        {
            if ( format.Extension == extension )
                return &format;
        }
        return nullptr;
    }

    /// Would the automatic sweep generate a picture for this format? True only for the producers that
    /// MAKE a file: a decoded image is already its own picture and an authored one is a person's
    /// decision. Both of those must be false here, or the sweep would spend a renderer on a texture and
    /// invent a camera angle for a level.
    [[nodiscard]] constexpr bool IsGenerated( Producer by ) noexcept
    {
        return by == Producer::RenderedMaterial || by == Producer::RenderedMesh || by == Producer::Painted;
    }

    /// Does generating this picture claim one of the six renderer slots?
    ///
    /// THE WHOLE POINT OF ASKING IT HERE. The cloud formats are painted on a worker thread and claim
    /// nothing, so a project that is all clouds sweeps at full speed with every slot still free for the
    /// person; a project that is all meshes drains one capture at a time behind RendererSlotBudget. Two
    /// different costs, one table, and the sweep does not have to know which is which.
    [[nodiscard]] constexpr bool NeedsRendererSlot( Producer by ) noexcept
    {
        return by == Producer::RenderedMaterial || by == Producer::RenderedMesh;
    }
} // namespace Desert::Editor::ThumbnailFormats
