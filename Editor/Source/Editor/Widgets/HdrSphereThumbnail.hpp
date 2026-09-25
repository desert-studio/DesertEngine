#pragma once

#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace Desert::Editor::HdrSphereThumbnail
{
    /**
     * @file
     * @brief AN EQUIRECTANGULAR ENVIRONMENT MAP, SHOWN AS A BALL — ON THE CPU, WITH NO RENDERER SLOT.
     *
     * The Content Browser used to show a `.hdr` as its own latitude-longitude strip, decoded and squashed
     * into a square. The owner asked for what UE's Content Browser shows for a cube texture instead: a
     * sphere with the environment on it. The earlier refusal (ThumbnailFormats.hpp, 2026-09-23) priced
     * that sphere as the IBL bake plus a renderer slot, 252-386 ms per tile — which is the price of
     * LIGHTING a ball BY the environment. Wrapping the map ONTO the ball is a different and much smaller
     * thing: every pixel of the disc has a sphere normal, the normal is a direction, and a direction is
     * one bilinear lookup in the map. No bake, no device, no slot — a pure function over the file's bytes,
     * which is exactly the property every other `Producer::Painted` row has.
     *
     * WHY THE NORMAL AND NOT THE REFLECTION. A mirror ball (the reflected view ray) shows the environment
     * BEHIND the viewer, squeezes the whole back hemisphere into the rim and puts the part of the map the
     * camera faces nowhere. The normal shows the map as it is painted on a globe: the patch facing the
     * camera sits in the middle of the disc, undistorted, which is what makes two thumbnails of two
     * captures distinguishable at 64 px. Desert/Tests/Editor/ThumbnailFormats pins this: a reflected
     * lookup fails its off-centre probes.
     *
     * CONVENTIONS, stated once because both the painter and its test derive from them:
     *   * The camera orbits the ball's centre. At yaw 0 / pitch 0 it sits on +Z looking toward -Z, with
     *     +X to the right of the image and +Y up; positive yaw turns it toward +X, positive pitch raises
     *     it. So the disc's centre pixel shows the map in the direction of @ref CameraDirection.
     *   * The map is read with the engine's own equirectangular convention (see @ref DirectionToUv), so
     *     the ball shows the side of the sky the skybox would show in that direction.
     */

    /// A decoded equirectangular map: linear RGB floats, `Width * Height * 3`, first row is the TOP
    /// (the zenith), as stb_image and every Radiance writer store it.
    struct EquirectMap
    {
        uint32_t           Width  = 0;
        uint32_t           Height = 0;
        std::vector<float> Rgb;
    };

    /// Where the camera looks at the ball from. The painter uses @ref kThumbnailView; the test passes its
    /// own, so a view whose geometry is known by hand can be checked by hand.
    struct SphereView
    {
        float YawDegrees   = 0.0f;
        float PitchDegrees = 0.0f;
    };

    /// The preview viewport's "Three-Quarter" (Editor/Core/PreviewViewpoints.hpp): a tile from the same
    /// angle as the live ball the Details Skybox section orbits, so the two read as one object.
    inline constexpr SphereView kThumbnailView{ 35.0f, 20.0f };

    /// The ball's radius as a fraction of HALF the side. Below 1 so the silhouette, and therefore the
    /// fact that it is a sphere and not a cropped photograph, reads against the backdrop.
    inline constexpr float kDiscRadiusFraction = 0.86f;

    /// Unit vector from the ball's centre toward the camera.
    [[nodiscard]] glm::vec3 CameraDirection( const SphereView& view );

    /// The engine's direction -> equirectangular UV: `PanoramaSampleUV` from
    /// Editor/Resources/Shaders/Common/SkyPanorama.glslh, compiled here as C++ — the same text the
    /// PanoramaToCubemap bake reads the file with, so the ball and the skybox cannot disagree about which
    /// texel a direction is. u runs around the horizon, v = 0 is straight up (+Y).
    [[nodiscard]] glm::vec2 DirectionToUv( const glm::vec3& direction );

    /// Bilinear lookup of @p map in @p direction, wrapping around the seam in u and clamping at the poles.
    [[nodiscard]] glm::vec3 Sample( const EquirectMap& map, const glm::vec3& direction );

    /// Linear HDR radiance -> display byte: exposure 1, the ACES fit SceneComposite applies by
    /// default (decision D-10), then the sRGB encode the swapchain would apply.
    [[nodiscard]] std::array<unsigned char, 3> ToDisplay( const glm::vec3& linear );

    /// The picture: RGBA8, `side * side * 4`, top row first, opaque everywhere (the grid composites over
    /// panels of different colours, so the backdrop is painted, not left transparent).
    [[nodiscard]] std::vector<unsigned char> Paint( const EquirectMap& map, uint32_t side,
                                                    const SphereView& view );

    /// Decode a Radiance `.hdr` held in memory. Refuses by name a payload that is not an HDR image or does
    /// not decode — a blank ball for a broken file would be called fresh by the thumbnail cache for ever.
    [[nodiscard]] Common::ResultStr<EquirectMap> Decode( const std::vector<unsigned char>& payload );
} // namespace Desert::Editor::HdrSphereThumbnail
