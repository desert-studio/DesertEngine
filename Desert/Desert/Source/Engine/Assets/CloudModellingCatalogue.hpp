#pragma once

#include <Engine/Assets/CloudModellingVolume.hpp>

#include <cstdint>

namespace Desert::Assets
{
    /**
     * @file
     * @brief The CATALOGUE OF FORMS phase Э4 is measured by — ten genera, each written as a recipe the
     *        sculpting panel can open and edit.
     *
     * `Docs/Clouds/ANALYSIS_APPROACH.md` §6 names the list and says what it is for: "a genus that is
     * indistinguishable from its neighbour is a caption, not a form". So these are not ten files, they are
     * ten SHAPES, and `Desert/Tests/Engine/CloudCatalogue` measures each one against the property that
     * makes it that genus — an anvil wider than the tower under it, a sheet fifteen times wider than it is
     * thick, an arch that is one connected body with a hole through it.
     *
     * WHY RECIPES IN CODE AND NOT TEN `.dcmv` FILES IN THE TREE. A baked body is 4.00 MiB; ten of them is
     * 40 MiB of binary in a repository, permanently, for shapes an artist is expected to edit rather than
     * use as shipped. A recipe is two hundred bytes. `Tools/CloudVolumeBaker --catalogue <name>` turns one
     * into a file, and the sculpting panel's Catalogue button drops one into the lump list where it can be
     * changed — which is the point, since the catalogue is a STARTING POINT for an artist and not a
     * library of finished clouds. The two bodies the demo scenes need are baked and committed; the other
     * eight are one command away.
     *
     * THE GENUS NAMES ARE THE ONES THE `.decloudtype` FILES UNDER `Projects/Desert/Content/Clouds/Types` ALREADY
     * USE, and the two halves are deliberate counterparts: a cloud TYPE (phase T) says at what altitude and with
     * what vertical profile the PROCEDURAL producer lays a genus down; a catalogue entry says what one
     * body of that genus is SHAPED like. A scene that wants a cumulonimbus sky and a cumulonimbus hero
     * cloud names the two by the same word, which is the whole reason the vocabularies were kept in step.
     *
     * WHAT THIS CANNOT DO, stated here rather than discovered. The cauliflower surface of p.69 of the deck
     * comes out of a fluid solver, and decision §6 of the analysis is that there will be no solver. Lumps
     * plus the up-rez noise give a SILHOUETTE with an eroded edge; they do not give the recursive
     * self-similar boil of a real convective turret. Every entry below is affected and the congestus and
     * the cumulonimbus most of all.
     */

    /// The ten genera, in the order §6 of the analysis lists them. The values are stable — they are
    /// written into no file, but the sculpting panel's dropdown is indexed by them and a test pins the
    /// count, so a genus inserted in the middle is a change somebody has to mean.
    enum class CloudModellingSpecies : uint32_t
    {
        CumulusHumilis   = 0,
        CumulusMediocris = 1,
        CumulusCongestus = 2,
        Cumulonimbus     = 3,
        Stratocumulus    = 4,
        Stratus          = 5,
        Altocumulus      = 6,
        Cirrus           = 7,
        Lenticular       = 8,
        Freeform         = 9,
    };

    inline constexpr uint32_t kCloudModellingSpeciesCount = 10u;

    /// What to call the genus in a panel, a log line or a report.
    const char* CloudModellingSpeciesName( CloudModellingSpecies species );

    /// The file stem `Tools/CloudVolumeBaker --catalogue` writes and the panel's dropdown matches against
    /// — lower case, no spaces, no extension.
    const char* CloudModellingSpeciesKey( CloudModellingSpecies species );

    /// The recipe for that genus. A reference to a static: every one of them is a constant, and returning
    /// a copy of a vector of lumps from a dropdown's redraw would be a waste nobody would ever notice and
    /// nobody should have to pay.
    const CloudModellingVolumeRecipe& CloudModellingCatalogueRecipe( CloudModellingSpecies species );
} // namespace Desert::Assets
