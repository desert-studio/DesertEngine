#pragma once

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

namespace Desert::Graphic
{
    // ONE IES LIGHT PROFILE AS THE SHADERS SAMPLE IT (UE UTextureLightProfile + FIESAtlas): an IESNA LM-63
    // photometric file resampled onto a fixed grid of candela, normalised to its peak. Columns run over the
    // VERTICAL angle (0 = along the light's forward, 180 = straight back), rows over the HORIZONTAL angle about
    // the forward axis (0 along the light's bitangent, 90 along its tangent; Mesh/IesProfile.glslh spells the
    // same frame), the last row repeating the first so a clamped bilinear fetch wraps without a seam.
    //
    // UE keeps a profile as a 256-wide texture per profile and packs them into an atlas whose rows are the
    // profiles. A row per profile loses the horizontal angle (an asymmetric street-lamp profile then reads as
    // its 0-degree plane all round), so each profile here is a block of kIesProfileRows rows.
    inline constexpr uint32_t kIesProfileColumns = 256;
    inline constexpr uint32_t kIesProfileRows    = 32;

    struct IesProfileGrid
    {
        // kIesProfileRows x kIesProfileColumns, row-major, in [0, 1] (the candela over the profile's peak).
        std::vector<float> Candela;
        // The peak candela of the file (the candela multiplier applied): UE's "IES brightness".
        float PeakCandela = 0.0f;
    };

    // The photometric data of an LM-63 file (1986, 1991, 1995 and 2002 revisions): Type C photometry, the
    // angles in degrees as the file lists them, candela[horizontal][vertical] with the multiplier applied.
    struct IesPhotometricData
    {
        std::vector<float> VerticalAngles;
        std::vector<float> HorizontalAngles;
        std::vector<float> Candela;
    };

    // Parses the text of a .ies file. Refused: no TILT line, a TILT=<file> that is not INCLUDE (the tilt
    // file is not part of the profile), fewer numbers than the header announces, angles that do not rise, a
    // photometric type other than C (A and B are vehicle and floodlight conventions with another frame).
    [[nodiscard]] Common::ResultStr<IesPhotometricData> ParseIesProfile( std::string_view text );

    // The data on the shaders' grid, with Type C symmetry expanded: one horizontal angle = rotationally
    // symmetric, 0..90 = quadrant-symmetric, 0..180 = bilateral, 0..360 = full. Directions the file does not
    // cover (above the horizon of a 0..90 vertical range) emit nothing. Refused: a profile whose peak is 0.
    [[nodiscard]] Common::ResultStr<IesProfileGrid> ResampleIesProfile( const IesPhotometricData& data );
} // namespace Desert::Graphic
