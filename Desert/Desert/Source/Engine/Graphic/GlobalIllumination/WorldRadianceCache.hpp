#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/GlobalIllumination/RadianceCache.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

// THE WORLD RADIANCE CACHE, RUNNING (GI-RADIANCE-CACHE) — the update of RadianceCache.hpp's interface, ported from
// UE Lumen's LumenRadianceCache.cpp / LumenRadianceCacheUpdate.usf / LumenRadianceCache.usf. Six compute passes a
// frame (Programs/GlobalIllumination/RadianceCache*.shader):
//   Mark      the cells this frame's readers interpolate are stamped "used" (UE: by the screen probes; until
//             GI-FINAL-GATHER the readers are the G-buffer pixels themselves).
//   Update    entries whose probe belongs to another world cell (the clipmap scrolled) or that went unread for
//             kProbeUnusedFramesBeforeFree frames free their slot.
//   Allocate  used empty entries take a slot; every used probe is ranked (new, lights changed, then age of its
//             last trace) into the priority histogram.
//   Select    the histogram is cut at ProbesPerFrame; the probes above the cut are this frame's trace list.
//   Trace     each listed probe traces its octahedral texels through the GI ray contract (Common/GITracing.glslh:
//             the global distance field, the hit lit in place, the sky on a miss) into the radiance atlas, blended
//             with its history.
//   Filter    radiance -> irradiance probe and distance moments, octahedral borders; the probe is published.
// The deferred composite then reads the irradiance at each pixel (RadianceCacheIrradianceAt) in place of the sky's
// diffuse irradiance (decision 7 of GI-ALL-LIGHTS: the trace sees the sky where the sky is visible).
//
// WHERE THIS DIFFERS FROM UE (for the owner):
//   * Visibility in the interpolation. UE interpolates the eight probes with trilinear weights only and leaves
//     leaks to the screen probes' short traces in front of the cache. Until those exist the pixel reads the cache
//     directly, so each probe also keeps the mean and mean square of its rays' hit distances (DDGI), and a probe
//     that sees a nearer surface in the point's direction (the point is behind a wall from it) is weighted out
//     by Chebyshev's bound. Kept when screen probes arrive: it also guards their own cache reads.
//   * Probes move out of geometry before they trace (DDGI's relocation, along the distance field's gradient,
//     at most 0.45 of a cell). UE's world-grid probe inside a wall traces from inside it.
//   * Lights changing in a probe's cell raise its priority (RadianceCache.hpp header note).
//   * The indirection is a toroidal BUFFER (world cell mod resolution), not UE's 3D texture remapped on every
//     clipmap move: a scroll costs nothing, an entry is stale exactly when its probe records another cell.
//   * A probe keeps its slot kProbeUnusedFramesBeforeFree frames unread; UE frees it the first unmarked frame.
namespace Desert::ShaderResources
{
    class StorageBuffer;
}

namespace Desert::Graphic
{
    class Image;
    class Image2D;
    class Shader;
    class ComputePipeline;
} // namespace Desert::Graphic

namespace Desert::Graphic::GlobalIllumination
{
    // std430 — must match `struct GIFrame` in Common/GIFrame.glslh. 288 bytes.
    struct GIFramePayload
    {
        glm::mat4 ViewProjection{ 1.0f };    // the jittered matrix the G-buffer was rasterised with
        glm::mat4 InvViewProjection{ 1.0f }; // its inverse
        glm::vec4 Camera{ 0.0f };            // xyz = camera world position (cm)
        glm::vec4 SunTravel{ 0.0f };         // xyz = direction the sunlight travels, w = 1 when there is a sun
        glm::vec4 SunRadiance{ 0.0f };       // rgb = colour * intensity
        glm::vec4 SkyYawCosSin{ 1.0f, 0.0f, 0.0f, 0.0f };
        glm::vec4 SkyGain{ 1.0f };
        glm::vec4 GdfLevels[4]{}; // GlobalDistanceField::FrameField::Levels
        glm::vec4 Screen{ 0.0f }; // xy = G-buffer extent (pixels), zw = 1 / that
    };
    static_assert( sizeof( GIFramePayload ) == 288 );

    // This frame's graph resources the update reads.
    struct RadianceCacheUpdateInputs
    {
        RDG::TextureRef                GBufferDepth;
        RDG::TextureRef                GBufferA; // albedo
        RDG::TextureRef                GBufferB; // normal
        RDG::TextureRef                GBufferEmissive;
        RDG::TextureRef                EnvSpecular; // the sky's radiance cube (or the system black cube)
        std::array<RDG::TextureRef, 4> GlobalDistanceField;
        RDG::BufferRef                 PointLights;
        RDG::BufferRef                 SpotLights;
        RDG::BufferRef                 WorldGridParams; // the world light grid (LightGridLayout::WorldClipmaps)
        RDG::BufferRef                 WorldGridCells;
        GIFramePayload                 Frame;
        glm::vec3                      CameraPosition{ 0.0f };
        glm::uvec2                     Extent{ 0u };
    };

    // What a reader of this frame's cache binds (Common/RadianceCache.glslh, the read section).
    struct RadianceCacheFrame
    {
        RDG::BufferRef  Params;      // "RadianceCacheParamsUB"
        RDG::BufferRef  Indirection; // "RadianceCacheIndirectionUB"
        RDG::BufferRef  Probes;      // "RadianceCacheProbesUB"
        RDG::TextureRef Irradiance;  // "u_RadianceCacheIrradiance"
        RDG::TextureRef Depth;       // "u_RadianceCacheDepth"
    };

    class WorldRadianceCache
    {
    public:
        WorldRadianceCache();
        ~WorldRadianceCache();
        WorldRadianceCache( const WorldRadianceCache& )            = delete;
        WorldRadianceCache& operator=( const WorldRadianceCache& ) = delete;

        // The six shaders and pipelines. An error names what is missing.
        [[nodiscard]] Common::BoolResultStr Initialize();

        using ImportImage = std::function<RDG::TextureRef( const std::shared_ptr<Image>&, std::string_view )>;

        // The update of one frame, its six passes added to @p graph. The cache is (re)built when @p shape differs
        // from the last frame's (a quality change): every probe is traced again.
        [[nodiscard]] Common::ResultStr<RadianceCacheFrame>
        AddUpdatePasses( RDG::Builder& graph, const ImportImage& import,
                         const RadianceCache::RadianceCacheInputs& shape,
                         const RadianceCacheUpdateInputs&          inputs );

    private:
        enum Pass : uint32_t
        {
            Mark = 0,
            Update,
            Allocate,
            Select,
            Trace,
            Filter,
            PassCount
        };

        [[nodiscard]] Common::BoolResultStr EnsureResources( const RadianceCache::RadianceCacheInputs& shape );

        std::array<std::shared_ptr<Shader>, PassCount>          m_Shaders;
        std::array<std::shared_ptr<ComputePipeline>, PassCount> m_Pipelines;
        mutable std::array<ShaderBindingLayoutCache, PassCount> m_Layouts;

        // Persistent state (StorageBuffer persistent = true: one device buffer every frame writes).
        std::shared_ptr<ShaderResources::StorageBuffer> m_Indirection;
        std::shared_ptr<ShaderResources::StorageBuffer> m_LastUsed;
        std::shared_ptr<ShaderResources::StorageBuffer> m_Probes;
        std::shared_ptr<ShaderResources::StorageBuffer> m_FreeList;
        RDG::ExternalBuffer                             m_IndirectionImport;
        RDG::ExternalBuffer                             m_LastUsedImport;
        RDG::ExternalBuffer                             m_ProbesImport;
        RDG::ExternalBuffer                             m_FreeListImport;
        std::shared_ptr<Image2D>                        m_RadianceAtlas;
        std::shared_ptr<Image2D>                        m_IrradianceAtlas;
        std::shared_ptr<Image2D>                        m_DepthAtlas;

        RadianceCache::RadianceCacheInputs m_Shape{};
        bool                               m_HasShape   = false;
        bool                               m_Reset      = true;
        uint32_t                           m_FrameIndex = 0;
    };
} // namespace Desert::Graphic::GlobalIllumination
