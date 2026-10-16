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
#include <vector>

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
    ///  - Whole: every step in the frame that begins it. A scene's FIRST capture is whole, so the frame that opens
    ///    the view already lights its scene with its sky (UE captures the first real-time sky light complete),
    ///    and so is the first that carries its clouds (Graphic::SkyCaptureIsWhole).
    ///  - TimeSliced: one slice per frame (a band of a face, see SkyCaptureFaceSlices, then the filters). A
    ///    RECAPTURE (the sun, the clouds or the sky changed) is sliced, and the environment the scene reads
    ///    meanwhile is the previous one, complete, so there is never a frame without sky light: the new one
    ///    replaces it in the frame its last slice is recorded.
    enum class SkyCapturePace : uint8_t
    {
        Whole,
        TimeSliced,
    };

    /// The steps of one capture, in recording order. One step is one slice of a TimeSliced capture, except Faces,
    /// which is one SkyCaptureFaceSlice per slice (SkyCaptureFaceSlices).
    enum class SkyCaptureStep : uint8_t
    {
        Faces,  // the atmosphere and its clouds rendered straight into the radiance cube's faces at the capture
                // size (BakeProceduralSky), UE's real-time capture: no panorama, no resample
        Filter, // the radiance cube's mip chain (2x2 box per mip), the diffuse irradiance, summed exactly over
                // its 16-texel mip (SkyCaptureIrradiance), and the GGX-prefiltered cube, every mip
                // (PrefilterEnvMap at UE's real-time sample count). One slice: at the capture's face the three
                // are cheap, and the chain ends the slice read whole, in one layout.
        Done,
    };

    /// HOW A TIME-SLICED CAPTURE SPLITS ITS FACES: the rows of a face that can see the sky into this many bands
    /// of the face's height, one band per frame; the rows that look below the horizon (the lower half of a side
    /// face, the whole down face), which cost two sphere tests a ray, in one slice per face. UE slices its
    /// real-time capture per face (r.SkyLight.RealTimeReflectionCapture.TimeSlice); here a face is too much
    /// for one frame once the clouds are in it, because the capture marches them at the SCREEN's sample counts.
    /// It must: the multiple-scattering octaves are integrated over a step with their reduced extinction
    /// (Common/CloudLighting.glslh, CloudMultiScatterStep), so a cloud marched in fewer, longer steps is
    /// brighter -- measured on Clouds_Protocol (128 face, the ground's mean grey level against the panorama
    /// path's 90.92): 256 steps 90.86, 64 steps 99.87, 32 steps 109.34, 16 steps 119.60; with one octave 32
    /// and 256 steps give 78.11 and 78.74. A cheaper march would light the world with brighter clouds than the
    /// camera sees, so the cost is spread over frames instead. Whole faces measured 50-97 GPU ms on an M1 Pro
    /// (shared), the up face the dearest; a sixteenth of the height fits a frame.
    inline constexpr uint32_t kSkyCaptureBandsPerFace = 16u;
    inline constexpr uint32_t kSkyCaptureFaceCount    = 6u;

    /// One slice of a time-sliced capture's faces step: rows [FirstRow, FirstRow + Rows) of one face.
    struct SkyCaptureFaceSlice
    {
        uint32_t Face     = 0u;
        uint32_t FirstRow = 0u;
        uint32_t Rows     = 0u;
    };

    /// The faces step of a time-sliced capture at @p face texels, in recording order (see
    /// kSkyCaptureBandsPerFace). The faces are PanoramaToCubemap's: 0 +X, 1 -X, 2 +Y (up), 3 -Y (down), 4 +Z, 5
    /// -Z, row 0 at the top, so a side face's rows from face / 2 down look below the horizon.
    [[nodiscard]] std::vector<SkyCaptureFaceSlice> SkyCaptureFaceSlices( uint32_t face );

    /// What the faces step marches, fixed when the capture begins: the faces are the capture's only step that
    /// reads the sky and its clouds. The buffers and volumes named here are bound once, at Begin, and read by
    /// every face slice after it -- so the capture keeps Clouds (its shared references) until it completes or
    /// is abandoned, and a slice never reads a volume the cloud renderer has since released.
    struct SkyCaptureSource
    {
        // The sky's parameter block -- the one the screen sky pass reads (SkyboxRenderer::m_SkyParams).
        ShaderResources::StorageBuffer* SkyParams = nullptr;
        // The cloud layer marched into the faces (bound through the pipeline route: assets and the cloud
        // renderer's volumes, all written by earlier frames).
        CloudBakeBinding Clouds;
        // The physical model marches the atmosphere LUTs; the gradient model samples none.
        bool Physical = false;
    };

    /// The images the faces step SAMPLES that this frame's graph may write before it (the atmosphere's
    /// cached LUT pair and the distant sky light, SkyboxRenderer::DeclareAtmosphereCachedLutNodes). Declared as
    /// reads of the step, so the graph orders the step after their writers and places the barriers. A null one
    /// is bound to the engine's fallback image, as the bake always did.
    struct SkyCaptureFrameReads
    {
        // The recording view's sky block, written by that view this frame (a per-frame-in-flight buffer): a face
        // slice binds it rather than the block of the view that began the capture, which may not record this
        // frame, or exist.
        ShaderResources::StorageBuffer* SkyParams = nullptr;
        std::shared_ptr<Image2D>        TransmittanceLut;
        std::shared_ptr<Image2D>        MultiScatterLut;
        std::shared_ptr<Image2D>        DistantSkyLight;
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
     * scene's sky light (SceneSkyLight) publishes stays the previous capture's until this one's last step is
     * recorded, and is then replaced in the same frame (SceneSkyLight::AddPasses), before ImportSceneViewTextures
     * imports it. UE does the same with its two convolved sky targets and their ready index.
     *
     * One capture at a time, one per scene: SceneSkyLight owns it and the environment it publishes, the claiming
     * view's SkyboxRenderer decides WHEN (SkyRules: the sun, the clouds, the sky's own parameters), and this
     * object only HOW.
     */
    class SkyCapture
    {
    public:
        SkyCapture() = default;
        ~SkyCapture();
        SkyCapture( const SkyCapture& )            = delete;
        SkyCapture& operator=( const SkyCapture& ) = delete;

        /// The three fixed pipelines (the faces' is per cloud medium, made by Begin).
        [[nodiscard]] Common::BoolResultStr Initialize();

        /// Allocates a capture at @p size: its radiance cube and the two cubes it will publish, all of @p size's
        /// face. The first faces are recorded by this frame's AddPasses. Refused, with the reason logged, when a
        /// capture is already running or an image or the faces' program is missing.
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
        /// (logged), and the environment the scene reads is kept.
        [[nodiscard]] std::optional<Environment> Settle( bool accepted );

        /// What the running capture is, for the landing line: its face size.
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
        // The faces' program for the cloud layer's medium (a variant when the medium is authored), kept while it
        // is the one in use and for the captures after it, see m_RetiredFaces.
        [[nodiscard]] bool AcquireFacesProgram( const CloudBakeBinding& clouds );
        // The pipeline route of the faces step: the sky block and the cloud layer, every declared slot written
        // (fallbacks included) -- an unwritten one invalidates the set and loses the whole environment.
        void BindFacesRoute( const SkyCaptureSource& source );
        void ReleaseCapture();

        // Rows [firstRow, firstRow + rowCount) of faces [firstFace, firstFace + faceCount) of the radiance cube's
        // mip 0, one dispatch.
        void AddFaces( RDG::Builder& graph, FrameTextures& textures, const SkyCaptureFrameReads& reads,
                       uint32_t firstFace, uint32_t faceCount, uint32_t firstRow, uint32_t rowCount );
        void AddRadianceMips( RDG::Builder& graph, FrameTextures& textures );
        void AddIrradiance( RDG::Builder& graph, FrameTextures& textures );
        void AddPrefilter( RDG::Builder& graph, FrameTextures& textures );

        Program m_Downsample;
        Program m_Irradiance;
        Program m_Prefilter;
        Program m_Faces;
        // Faces programs replaced by a medium change, kept for a few captures: the dispatches of a frame still in
        // flight were recorded with them.
        std::deque<Program> m_RetiredFaces;

        // THE CAPTURE IN PROGRESS. The radiance cube is its intermediate (released when it completes; 0.5 MiB at
        // the 128 face); the two cubes it publishes are registered in the ImageService from the start, because
        // the environment the lit passes read names them by handle.
        std::shared_ptr<ImageCube> m_RadianceImage;
        Runtime::ImageHandle       m_IrradianceHandle;
        Runtime::ImageHandle       m_PrefilteredHandle;
        // The intermediate of the capture that completed most recently: this frame's graph imported it, so it is
        // released when the next capture begins rather than while that graph is still being built.
        std::shared_ptr<ImageCube> m_RetiredRadianceImage;

        SkyEnvironmentSize m_Size{};
        SkyCapturePace     m_Pace = SkyCapturePace::Whole;
        SkyCaptureStep     m_Next = SkyCaptureStep::Done;
        // The time-sliced faces step's slices, the next one the Faces step renders, and the one after the last
        // AddPasses recorded.
        std::vector<SkyCaptureFaceSlice> m_FaceSlices;
        uint32_t                         m_FaceSlice         = 0u;
        uint32_t                         m_RecordedFaceSlice = 0u;
        // The steps the last AddPasses recorded, [m_Next, m_RecordedEnd); Settle advances to the end.
        SkyCaptureStep m_RecordedEnd = SkyCaptureStep::Done;
        // The cloud layer the running capture marches, co-owned from Begin to its last face slice (see
        // SkyCaptureSource): the faces' pipeline route names these by raw descriptor.
        CloudBakeBinding m_Clouds;
        // The faces' push block (x marches, y reads sky occlusion, z aerial start km, w per-sample sun).
        glm::vec4 m_CloudPush{ 0.0f };
        bool      m_Physical = false;
    };
} // namespace Desert::Graphic::System
