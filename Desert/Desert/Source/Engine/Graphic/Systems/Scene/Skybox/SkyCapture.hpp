#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/Clouds/CloudEnvironmentBake.hpp>
#include <Engine/Graphic/Environment/SceneEnvironment.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/SkyRules.hpp>
#include <Engine/Runtime/ImageHandle.hpp>

#include <glm/glm.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>

namespace Desert::ShaderResources
{
    class StorageBuffer;
}

namespace Desert::Graphic
{
    class FrameTextures;
    namespace RDG
    {
        class Builder;
    }
} // namespace Desert::Graphic

namespace Desert::Graphic::System
{
    /// HOW A CAPTURE IS SPREAD OVER FRAMES (UE: r.SkyLight.RealTimeReflectionCapture.TimeSlice).
    ///  - Whole: every step in the frame that begins it. A view's FIRST capture is whole, so the frame that opens
    ///    the view already lights its scene with its sky (UE captures the first real-time sky light complete).
    ///  - TimeSliced: one step per frame. A RECAPTURE (the sun, the clouds or the sky changed) is sliced, and the
    ///    environment the scene reads meanwhile is the previous one, complete, so there is never a frame without
    ///    sky light: the new one replaces it in the frame its last step is recorded.
    enum class SkyCapturePace : uint8_t
    {
        Whole,
        TimeSliced,
    };

    /// The steps of one capture, in recording order. One step is one slice of a TimeSliced capture.
    enum class SkyCaptureStep : uint8_t
    {
        Panorama, // the atmosphere and its clouds marched into an equirect panorama (BakeProceduralSky)
        Radiance, // the panorama resampled into the radiance cube, then its mip chain (2x2 box per mip)
        Filter,   // the diffuse irradiance, summed exactly over a coarse radiance mip (SkyCaptureIrradiance), and
                // the GGX-prefiltered cube, every mip (PrefilterEnvMap at UE's real-time sample count)
        Done,
    };

    /// What the panorama step marches, fixed when the capture begins: the panorama is the capture's only step
    /// that reads the sky and its clouds, and it is recorded in the frame the capture begins, so every value
    /// here is that frame's.
    struct SkyCaptureSource
    {
        // The sky's parameter block -- the one the screen sky pass reads (SkyboxRenderer::m_SkyParams).
        ShaderResources::StorageBuffer* SkyParams = nullptr;
        // The cloud layer marched into the panorama (bound through the pipeline route: assets and the cloud
        // renderer's volumes, all written by earlier frames).
        CloudBakeBinding Clouds;
        // The physical model marches the atmosphere LUTs; the gradient model samples none.
        bool Physical = false;
    };

    /// The images the panorama step SAMPLES that this frame's graph may write before it (the atmosphere's
    /// cached LUT pair and the distant sky light, SkyboxRenderer::DeclareAtmosphereCachedLutNodes). Declared as
    /// reads of the step, so the graph orders the step after their writers and places the barriers. A null one
    /// is bound to the engine's fallback image, as the bake always did.
    struct SkyCaptureFrameReads
    {
        std::shared_ptr<Image2D> TransmittanceLut;
        std::shared_ptr<Image2D> MultiScatterLut;
        std::shared_ptr<Image2D> DistantSkyLight;
    };

    /**
     * @brief THE SKY LIGHT'S CAPTURE, RECORDED IN THE FRAME GRAPH (UE: the real-time SkyLight capture --
     * FScene::AllocateAndCaptureFrameSkyEnvMap, ReflectionEnvironmentRealTimeCapture.cpp).
     *
     * WHAT IT REPLACED: a GpuBatch of its own, submitted outside the frame and polled until its fence signalled.
     * Every new view (a scene open, a reflection-capture face view, an asset preview) drew its first 0.5 to
     * 4.3 s with a black sky ambient, and a physical sky waited one more frame for the atmosphere LUTs the graph
     * writes. Here the capture's dispatches are passes of the frame that needs them, recorded before the lit
     * passes that read the cubes, after the LUT nodes they march -- the graph orders and barriers them.
     *
     * TWO SETS, NEVER A GAP. The cubes a capture writes are not the ones the scene reads: the environment the
     * SkyboxRenderer publishes stays the previous capture's until this one's last step is recorded, and is then
     * replaced in the same frame (SkyboxRenderer::AddSkyCapturePasses), before ImportSceneViewTextures imports
     * it. UE does the same with its two convolved sky targets and their ready index.
     *
     * One capture at a time; the SkyboxRenderer decides WHEN (SkyRules: the sun, the clouds, the sky's own
     * parameters) and this object only HOW.
     */
    class SkyCapture
    {
    public:
        SkyCapture() = default;
        ~SkyCapture();
        SkyCapture( const SkyCapture& )            = delete;
        SkyCapture& operator=( const SkyCapture& ) = delete;

        /// The four fixed pipelines (the panorama's is per cloud medium, made by Begin).
        [[nodiscard]] Common::BoolResultStr Initialize();

        /// Allocates a capture at @p size: its panorama, radiance cube and the two cubes it will publish. The
        /// panorama step is recorded by this frame's AddPasses, so @p source is this frame's sky. Refused, with
        /// the reason logged, when a capture is already running or an image or the panorama program is missing.
        [[nodiscard]] bool Begin( const SkyEnvironmentSize& size, SkyCapturePace pace,
                                  const SkyCaptureSource& source );

        [[nodiscard]] bool IsCapturing() const
        {
            return m_Next != SkyCaptureStep::Done;
        }

        /// GRAPH BUILD: this frame's slice of the capture -- every remaining step when Whole, the next one when
        /// TimeSliced. Returns whether every pass was added; false adds none (an image the graph cannot import).
        [[nodiscard]] bool AddPasses( RDG::Builder& graph, FrameTextures& textures,
                                      const SkyCaptureFrameReads& reads );

        /// After AddPasses: @p accepted advances the capture past the steps it recorded, and the last one hands
        /// back the environment (its two cubes, which the caller now owns). A refused slice abandons the capture
        /// (logged): its panorama would be marched by a later frame than the stamps it was begun with.
        [[nodiscard]] std::optional<Environment> Settle( bool accepted );

        /// What the running capture is, for the landing line: its size, pace and how long it has run.
        [[nodiscard]] const SkyEnvironmentSize& GetSize() const
        {
            return m_Size;
        }

    private:
        struct Program
        {
            std::shared_ptr<ComputePipeline> Pipeline;
            ShaderBindingLayoutCache         Layout;
        };

        [[nodiscard]] static bool MakeProgram( Program& program, const char* shaderName );
        // The panorama program for the cloud layer's medium (a variant when the medium is authored), kept while
        // it is the one in use and for the captures after it, see m_RetiredPanorama.
        [[nodiscard]] bool AcquirePanoramaProgram( const CloudBakeBinding& clouds );
        // The pipeline route of the panorama step: the sky block and the cloud layer, every declared slot
        // written (fallbacks included) -- an unwritten one invalidates the set and loses the whole environment.
        void BindPanoramaRoute( const SkyCaptureSource& source );
        void ReleaseCapture();

        void AddPanorama( RDG::Builder& graph, FrameTextures& textures, const SkyCaptureFrameReads& reads );
        void AddRadiance( RDG::Builder& graph, FrameTextures& textures );
        void AddIrradiance( RDG::Builder& graph, FrameTextures& textures );
        void AddPrefilter( RDG::Builder& graph, FrameTextures& textures );

        Program m_PanoramaToCube;
        Program m_Downsample;
        Program m_Irradiance;
        Program m_Prefilter;
        Program m_Panorama;
        // Panorama programs replaced by a medium change, kept for a few captures: the dispatches of a frame still
        // in flight were recorded with them.
        std::deque<Program> m_RetiredPanorama;

        // THE CAPTURE IN PROGRESS. The panorama and the radiance cube are its intermediates (released when it
        // completes; the radiance cube is 128 MiB at the 1024 face); the two cubes it publishes are registered
        // in the ImageService from the start, because the environment the lit passes read names them by handle.
        std::shared_ptr<Image2D>   m_PanoramaImage;
        std::shared_ptr<ImageCube> m_RadianceImage;
        Runtime::ImageHandle       m_IrradianceHandle;
        Runtime::ImageHandle       m_PrefilteredHandle;
        // The intermediates of the capture that completed most recently: this frame's graph imported them, so
        // they are released when the next capture begins rather than while that graph is still being built.
        std::shared_ptr<Image2D>   m_RetiredPanoramaImage;
        std::shared_ptr<ImageCube> m_RetiredRadianceImage;

        SkyEnvironmentSize m_Size{};
        SkyCapturePace     m_Pace = SkyCapturePace::Whole;
        SkyCaptureStep     m_Next = SkyCaptureStep::Done;
        // The steps the last AddPasses recorded, [m_Next, m_RecordedEnd); Settle advances to the end.
        SkyCaptureStep m_RecordedEnd = SkyCaptureStep::Done;
        // The panorama's push block (x marches, y reads sky occlusion, z aerial start km, w per-sample sun).
        glm::vec4 m_CloudPush{ 0.0f };
        bool      m_Physical = false;
    };
} // namespace Desert::Graphic::System
