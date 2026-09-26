#pragma once

#include <Common/Core/ResultStr.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Desert::Editor::CloudThumbnail
{
    /**
     * @file
     * @brief THE FOUR CLOUD FORMATS, PHOTOGRAPHED WITHOUT A CAMERA.
     *
     * WHY THIS IS NOT A SceneRenderer. Every thumbnail this editor made before it was an offscreen RENDER
     * — a scene, a camera, a light, a whole view's GPU memory (Graphic/ViewResources.hpp) held
     * for the ~370 ms the capture takes. That is the right answer for a material and for a mesh, because
     * what those look like is a question about shading and geometry that only the renderer can answer.
     *
     * It is the WRONG answer for all four cloud formats, and the reason is a property of the files rather
     * than a preference: each one already contains the picture.
     *
     *   `.dclayout` carries its painted planes — RGBA8, 4*R*R bytes (Assets::CloudLayoutData::Pattern).
     *   `.dcnv`     carries baked voxels — RGBA8, 4*N^3, 8 MiB at the shipped N=128.
     *   `.dcmv`     carries baked voxels — RGBA8, 128*64*128, 4 MiB, and LOADING IT DOES NOT BAKE:
     *               `DecodeCloudModellingVolume` assigns the stored payload straight out of the container.
     *   `.decloudtype` carries the sampled SILHOUETTE — `Graphic::CloudVerticalProfile::HalfWidth`, the
     *               sixteen half-widths that are the species' shape.
     *
     * So the producer is a decode and a loop over bytes: no device, no slot, no main thread. That is why
     * ThumbnailService dispatches these to the JobSystem and keeps its single renderer for the two
     * formats that genuinely need one — a project made entirely of cloud assets sweeps at full speed with
     * all six slots still free for the surfaces a person opens.
     *
     * ── THE PHOTOGRAPH OF A CLOUD WAS REFUSED, AND HERE IS THE MEASUREMENT ────────────────────────────
     *
     * The alternative for `.decloudtype` in particular is obvious and was considered: render the species —
     * place a patch of it, march it, photograph the result. It was refused on numbers, and the numbers are
     * these:
     *
     *   * A `.decloudtype` is TWELVE NUMBERS AND A CURVE. It is not a volume; nothing in the file can be
     *     marched. Producing a photograph means BUILDING a volume from it first — the procedural bake,
     *     which is `GenerateCloudProceduralBlobs` plus a modelling volume, measured at ~1.6 s per lump
     *     optimised and tens of seconds in Debug (CloudModellingVolume.hpp:407, .hpp:441). The shipped
     *     library has nine types.
     *   * Then it must be MARCHED, which is the volumetric cloud pass — a full SceneRenderer, one of six
     *     slots, and the sky's own convergence window: `--shot-frames` below ~10 photographs the dither
     *     rather than the cloud (desert-engine-verify §1). So the cost is a bake plus a slot plus ten
     *     frames, per type, to fill nine tiles.
     *   * And it would be a photograph of a SAMPLE, not of the type: what a species looks like depends on
     *     the layer's coverage, its density scale, the sun angle and the layout bound to it. Two types
     *     photographed under one arbitrary sky differ by less than one type photographed under two skies.
     *
     * Against that, the silhouette below costs one JSON parse and a 512x512 fill — microseconds — and it
     * is the one thing that is a property of the FILE and of nothing else. What would change the answer:
     * if a type ever gains its own authored sky preset to be viewed under, the photograph stops being a
     * guess about the sky and becomes a picture of a stated thing, and it should be re-measured then.
     */

    /// The output PNG's side, in pixels. THE SAME NUMBER AssetThumbnailRenderer::kSize uses, and
    /// deliberately not an independent choice: `ThumbnailCache::kThumbMaxDim` is the size every thumbnail
    /// is uploaded at, so a picture written at any other size is either upscaled on screen or box-filtered
    /// away on every load. One size for every producer, so the grid cannot show two of them at two
    /// sharpnesses.
    inline constexpr uint32_t kSize = 512;

    /// THE PALETTE, once. Every painted producer draws on this backdrop — the four cloud formats, the theme
    /// strip and the HDR ball (HdrSphereThumbnail.cpp) — because a grid of tiles that do not share one reads
    /// as several kinds of asset rather than one family.
    inline constexpr std::array<unsigned char, 3> kBackdrop{ 24, 28, 34 };

    /**
     * @brief Paint the thumbnail for one cloud asset file and write it to @p png.
     *
     * SAFE ON A JobSystem WORKER: it opens two files and touches nothing else — no AssetManager, no ECS,
     * no GPU, no global mutable state. That is the whole reason it takes PATHS rather than an asset
     * handle; resolving a handle would drag the manager in and pin this to the main thread.
     *
     * REFUSES WITH THE REASON. An unsupported extension, a file that will not open, a container that will
     * not decode, a payload the format says should be there and is not — each returns an error naming
     * what was wrong. A blank square written on a failed decode would be the silent fallback §1.4 of the
     * contract forbids, and it is worse here than usual: the freshness rule would then call that blank
     * square fresh for ever.
     *
     * WRITES ASIDE AND RENAMES, exactly as AssetThumbnailRenderer does and for the same reason: an editor
     * that dies mid-write must not leave a truncated PNG at the cache path, because a truncated file is
     * "fresh" by modification time and undecodable for ever after.
     */
    [[nodiscard]] Common::BoolResultStr Write( const std::string& assetPath, const std::string& png );

    /**
     * @brief The pixels @ref Write would put in the file: RGBA8, `kSize * kSize * 4` bytes, top row first.
     *
     * SPLIT OUT SO THE PICTURE IS TESTABLE WITHOUT A FILESYSTEM ROUND TRIP. `Desert/Tests/Editor/
     * ThumbnailFormats` builds a `.dcnv` in memory, paints it, and asserts the square is not uniform —
     * which is the assertion that matters, because a producer that returns success and paints one flat
     * colour is the exact "empty successful answer" shape this whole subsystem keeps meeting.
     */
    [[nodiscard]] Common::ResultStr<std::vector<unsigned char>> Paint( const std::string& assetPath );
} // namespace Desert::Editor::CloudThumbnail
