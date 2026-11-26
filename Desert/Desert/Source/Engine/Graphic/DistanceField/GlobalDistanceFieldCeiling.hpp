#pragma once

#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>

#include <glm/glm.hpp>

#include <array>

namespace Desert::Graphic::DistanceField
{
    // The global distance field's clipmap levels (GlobalDistanceField.hpp; DistanceFieldShadowLayout.hpp sizes its
    // GdfLevels by it).
    inline constexpr int kGlobalDistanceFieldClipmaps = 4;

    // THE CEILING (FPS-SSR-2; UE Lumen's heightfield bounds / Frostbite hi-Z, as a max mip of the scene's height):
    // over the occupied box's XZ footprint a grid of kGdfCeilingCells^2 columns, each the highest Y (cm) any
    // placed record's box reaches over that column, both grown by the occupied box's margin
    // (kGdfOccupiedMarginVoxels of the coarsest clipmap: no clipmap sample above it, or beside it past the margin,
    // is in a march's band), and its max mip of kGdfCeilingCoarseCells^2 (blocks of kGdfCeilingMip^2 columns). A
    // far-field march ends after the last column whose ceiling its ray is not above
    // (Common/GITraceDistanceField.glslh GIFarFieldCeilingCut): the same proof as the occupied box, per column - a
    // ray that rose over the field, or skims over it above everything it will still cross, stops there. Made with
    // the placement (a scene/object change), never per frame. The GLSL struct GdfCeiling is generated from this
    // declaration (Generated/DistanceField/DistanceField.glslh, DistanceFieldLayout.hpp), read as the
    // giTracerGdfCeiling member of Common/GITracer.glslh's GITracerSoftwareUB.
    inline constexpr int kGdfCeilingCells       = 32;
    inline constexpr int kGdfCeilingMip         = 4;
    inline constexpr int kGdfCeilingCoarseCells = kGdfCeilingCells / kGdfCeilingMip;
    using GdfCeilingCoarseRows = std::array<glm::vec4, kGdfCeilingCoarseCells * kGdfCeilingCoarseCells / 4>;
    using GdfCeilingFineRows   = std::array<glm::vec4, kGdfCeilingCells * kGdfCeilingCells / 4>;
#define DESERT_GLOBAL_DISTANCE_FIELD_CEILING_FIELDS( X )                                                          \
    /* (min x, min z, cells per cm in x, cells per cm in z) of the fine grid; the coarse grid's cells per cm are  \
       those / kGdfCeilingMip. Cells per cm of 0: no ceiling (nothing placed) - the march is not cut. */          \
    X( glm::vec4, Grid, "Grid", glm::vec4( 0.0f ) )                                                               \
    /* Column ceilings, cm, 4 per vec4 in row-major (z rows of x): coarse, then fine. -1e30: an empty column. */  \
    X( GdfCeilingCoarseRows, Coarse, "Coarse", {} )                                                               \
    X( GdfCeilingFineRows, Fine, "Fine", {} )
    DESERT_SHADER_STRUCT( GlobalDistanceFieldCeiling, "GdfCeiling", Std430,
                          DESERT_GLOBAL_DISTANCE_FIELD_CEILING_FIELDS );
#undef DESERT_GLOBAL_DISTANCE_FIELD_CEILING_FIELDS
} // namespace Desert::Graphic::DistanceField
