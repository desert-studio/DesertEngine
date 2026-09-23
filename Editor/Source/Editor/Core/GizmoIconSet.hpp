#pragma once

// ── THE TEN VIEWPORT GIZMO ICONS, NAMED BY ROLE ────────────────────────────────────────────────────
//
// A viewport billboard used to be a glyph out of the Material Design icon FONT, picked at each call
// site by its picture. That had three consequences, and only the third was ever visible:
//
//   1. The mapping drifted from the meaning. `ICON_MDI_VIDEO` is a movie camera, not a photo camera;
//      a trigger volume was drawn as a map marker. Nothing could notice, because a glyph name says
//      what it LOOKS like and never what it MARKS.
//   2. One tone. A font glyph is a single coverage mask, so a gizmo could only ever be a symbol.
//   3. No census. Adding an entity kind meant remembering to pick a glyph somewhere in a 1400-line
//      overlay, and forgetting produced a plausible-looking wrong picture rather than a build error.
//
// This table is the fix for all three. The enumerator is the ROLE, the file is the artwork, and the
// two are bound in ONE place that a suite can walk: `Tests/Editor/GizmoIconSet` asserts every row has
// a file on disk and every .svg in the directory has a row, so artwork and code cannot drift apart
// silently. Swapping the picture later is a file replacement, not a rename through call sites.
//
// The artwork is Phosphor duotone (MIT; see Resources/Icons/Gizmo/NOTICE.md for why that set). Duotone
// is the load-bearing property: `Vector::BakeIconSdf` emits one SDF layer per colour run and
// `Runtime::IconService` keeps each layer's authored RGBA, so a two-tone icon reaches the viewport as
// two tinted draws of one atlas and stays vector-crisp at any billboard size.

#include <Common/Core/Constants.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace Desert::Runtime
{
    struct Icon;
}

namespace Desert::Editor
{
    // WHAT THE BILLBOARD MARKS. Ordered so the table below can be indexed by the enumerator, which is
    // what makes "every role has artwork" a static_assert instead of a hope.
    enum class GizmoIcon : size_t
    {
        LightPoint = 0,
        LightDirectional,
        LightSpot,
        Camera,
        AudioSource,
        TriggerVolume,
        SpawnPoint,
        Text,
        TransformEmpty,
        Script,
    };

    inline constexpr size_t kGizmoIconCount = static_cast<size_t>( GizmoIcon::Script ) + 1u;

    struct GizmoIconRow
    {
        GizmoIcon   Role;
        const char* File; // basename under Resources/Icons/Gizmo/
        const char* Label; // what a person calls this marker, for logs and the census's failure text
    };

    // THE ONE BINDING. A row's position IS its enumerator — asserted below rather than trusted.
    inline constexpr std::array<GizmoIconRow, kGizmoIconCount> kGizmoIcons{ {
         { GizmoIcon::LightPoint, "light-point.svg", "Point Light" },
         { GizmoIcon::LightDirectional, "light-directional.svg", "Directional Light" },
         { GizmoIcon::LightSpot, "light-spot.svg", "Spot Light" },
         { GizmoIcon::Camera, "camera.svg", "Camera" },
         { GizmoIcon::AudioSource, "audio-source.svg", "Audio Source" },
         { GizmoIcon::TriggerVolume, "trigger-volume.svg", "Trigger Volume" },
         { GizmoIcon::SpawnPoint, "spawn-point.svg", "Spawn Point" },
         { GizmoIcon::Text, "text.svg", "Text" },
         { GizmoIcon::TransformEmpty, "transform-empty.svg", "Empty" },
         { GizmoIcon::Script, "script.svg", "Script" },
    } };

    // The compile-time half of the census: the array is dense and in enum order, so `kGizmoIcons[(size_t)
    // role].Role == role` for every row. Without this the lookup below would silently answer with a
    // neighbour's artwork the first time somebody inserted an enumerator in the middle — the "middle link
    // drops a property" shape, and the cheapest possible place to close it.
    consteval bool GizmoIconTableIsDense()
    {
        for ( size_t i = 0; i < kGizmoIcons.size(); ++i )
            if ( static_cast<size_t>( kGizmoIcons[i].Role ) != i )
                return false;
        return true;
    }
    static_assert( GizmoIconTableIsDense(), "kGizmoIcons must be dense and in GizmoIcon order" );

    [[nodiscard]] inline const GizmoIconRow& GizmoIconRowOf( GizmoIcon role )
    {
        return kGizmoIcons[static_cast<size_t>( role )];
    }

    // The one subdirectory of Resources/Icons/ the gizmo set lives in.
    inline constexpr const char* kGizmoIconDir = "Gizmo";

    // Where the artwork lives, relative to the working directory the engine resolves Resources/ against.
    // A function and not a constant because Common::Constants::Path::ICONS_PATH follows a project remap.
    //
    // INLINE, and deliberately: the census suite walks this directory and must compile without the
    // graphics stack that Runtime::IconService (and therefore ResolveGizmoIcon below) pulls in. It
    // derives the directory from this function rather than spelling "Gizmo" a second time, so the census
    // asserts IDENTITY with what the editor loads instead of comparing two strings that agree today.
    [[nodiscard]] inline std::filesystem::path GizmoIconPath( GizmoIcon role )
    {
        return Common::Constants::Path::ICONS_PATH / kGizmoIconDir / kGizmoIcons[static_cast<size_t>( role )].File;
    }

    // The baked icon, imported on first use through Runtime::IconService (which owns the atlas every
    // layer's UVs address). nullptr when there is no icon service yet, or when the .svg is missing or
    // unparseable — the caller then draws the billboard's ring and halo without artwork rather than
    // taking the frame down. LAZY PER ROLE, not all ten at once: importing repacks and re-uploads the
    // atlas page, so a scene with one kind of light must not pay for nine icons it never draws.
    [[nodiscard]] const Runtime::Icon* ResolveGizmoIcon( GizmoIcon role );
} // namespace Desert::Editor
