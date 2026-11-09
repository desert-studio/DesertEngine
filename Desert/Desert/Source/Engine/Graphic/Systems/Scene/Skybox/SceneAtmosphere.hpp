#pragma once

#include <Common/Core/Memory/FrameArena.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RenderPassDeclaration.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/SkyPayload.hpp>
#include <Engine/Graphic/SkySettings.hpp>
#include <Engine/ShaderResources/StorageBuffer.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <optional>

namespace Desert::Graphic
{
    // The atmosphere LUT programs' shader parameters, one list for the scene's LUT nodes (SceneAtmosphere) and
    // the per-view fills that sample them (SkyboxRenderer).
    struct SkyboxLutReadShaderParameters
    {
        DESERT_SHADER_PARAMETER( Texture, TransmittanceLutTexture );
        DESERT_SHADER_PARAMETER( Sampler, GlobalFilterSampler );
        DESERT_SHADER_PARAMETER( Texture, MultiScatterLutTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, SkyViewLutTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, AerialPerspectiveLutTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, DistantSkyLightTexture );
        DESERT_SHADER_PARAMETER( Texture, CloudShadowMapTexture );
        DESERT_SHADER_PARAMETER( Sampler, LinearClampSampler );
    };

    struct SkyboxLutWriteShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageTexture, TransmittanceLutTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, MultiScatterLutTexture );
    };

    /**
     * @brief THE ATMOSPHERE OF ONE SCENE (UE: FSkyAtmosphereRenderSceneInfo with its view-independent LUTs, and
     * the sky light's real-time capture reading them -- FSkyLightSceneProxy +
     * ReflectionEnvironmentRealTimeCapture).
     *
     * Owned by the scene (Core::Scene), next to its sky light (SceneSkyLight). It holds every atmosphere LUT
     * PROGRAM (transmittance, multi-scattering, distant sky light, and the per-view Sky-View / aerial-perspective
     * fills the views record with them) and the LUTs that depend on the medium and the sun alone: the cached
     * transmittance 256x64 + multi-scattering 32x32 pair and the one-texel distant sky light.
     *
     * ONCE PER FRAME, BY THE FIRST VIEW. The view that claims the frame (ClaimFrame, the same rule SceneSkyLight
     * follows) uploads the atmosphere's own sky block (UploadSkyBlock) and records the LUT nodes at the head of
     * its graph (DeclareCachedLutNodes); the views after it read the images as that graph left them -- their
     * graphs execute after it on the same queue, and the images carry their layouts across graphs.
     *
     * THE CAPTURE'S OWN VIEW. The sky light's capture binds THIS block (GetSkyBlock) and THESE LUTs, never a
     * view's: the block is packed from the scene's sky and sun with no exposure ceiling (the bake does not read
     * it, SkyBakeFingerprint) and no camera (the capture marches from its own origin), so which view recorded a
     * frame changes nothing in the cube it captured.
     */
    class SceneAtmosphere
    {
    public:
        // Everything the two cached LUT passes read. When any of it moves, both LUTs are re-dispatched; while
        // it holds still, neither is: the sun and the bake knobs change constantly and none of them is a LUT
        // input. MieAnisotropy is also absent -- it rides in the payload for the integrators, but no LUT texel
        // depends on it.
        struct AtmosphereLutFingerprint
        {
            glm::vec3 RayleighScattering{ 0.0f };
            float     RayleighExpDistributionKm = 0.0f;
            glm::vec3 MieScattering{ 0.0f };
            glm::vec3 MieAbsorption{ 0.0f };
            float     MieExpDistributionKm = 0.0f;
            glm::vec3 OzoneAbsorption{ 0.0f };
            float     OzoneTipAltitudeKm = 0.0f;
            float     OzoneTipValue      = 0.0f;
            float     OzoneTentWidthKm   = 0.0f;
            glm::vec3 GroundAlbedo{ 0.0f };
            float     AtmosphereHeightKm    = 0.0f;
            float     MultiScatteringFactor = 0.0f;
            float     PlanetRadius          = 0.0f;

            bool operator==( const AtmosphereLutFingerprint& ) const = default;
        };
        static AtmosphereLutFingerprint LutFingerprintOf( const SkySettings& sky );

        // The block's LUT inputs: the packed block without the view's luminance ceiling (no LUT reads it).
        static uint64_t SkyBlockLutInputsOf( const SkyGpuPayload& payload );
        // THE INPUTS OF A FILL THAT FOLLOWS THE SKY BLOCK (CLOUDS-MARCH-LATENCY): the block, the generation of the
        // cached pair it samples and the dispatch's push. A fill whose inputs did not move is not re-marched.
        static uint64_t SkyLutFillInputsOf( uint64_t blockInputs, uint64_t pairGeneration, const glm::vec4& push );
        // FNV-1a continuation, for the fills that mix more into SkyLutFillInputsOf.
        static uint64_t MixBytes( uint64_t hash, const void* data, size_t size );

        static constexpr uint32_t kLutWorkGroupSize = 8; // LocalSize(8, 8, 1) in every 2D LUT shader
        static constexpr uint32_t LutGroupCount( uint32_t extent )
        {
            return ( extent + kLutWorkGroupSize - 1 ) / kLutWorkGroupSize;
        }
        static double LutBytesToMiB( uint64_t bytes )
        {
            return static_cast<double>( bytes ) / ( 1024.0 * 1024.0 );
        }

        SceneAtmosphere()                                    = default;
        SceneAtmosphere( const SceneAtmosphere& )            = delete;
        SceneAtmosphere& operator=( const SceneAtmosphere& ) = delete;

        /// The LUT programs and the atmosphere's sky block, made on first use (a scene that never shows a
        /// procedural sky makes none). False, said once, when the cached pair's programs are missing.
        [[nodiscard]] bool EnsurePrograms();

        /// Once per frame, by every view whose sky is procedural: true for the first one of this frame -- the
        /// view that uploads the block and records the LUT nodes -- and false for the others.
        [[nodiscard]] bool ClaimFrame( const void* view );
        [[nodiscard]] bool Claims( const void* view ) const;

        /// Every view of the scene, inside its ActiveViewScope, after ClaimFrame: the scene's sky as PackSky
        /// packed it, with the view's luminance ceiling cleared, into that view's copy of the block (per frame in
        /// flight x renderer slot) -- whichever view's graph binds it reads its own copy. The claiming view's
        /// upload is the one the LUT nodes' fill inputs are taken from.
        void UploadSkyBlock( const void* view, const SkyGpuPayload& payload );

        /// GRAPH BUILD, by @p view's graph, at its head: the cached pair when the medium changed, and the distant
        /// sky light when the block or the pair moved. Empty for a view that did not claim the frame.
        [[nodiscard]] ComputeNodeList DeclareCachedLutNodes( const void* view, Common::Memory::FrameArena& arena,
                                                             const SkySettings& sky );
        /// The cached pair counts as baked only once the frame graph accepted the nodes that bake it.
        void SettleCachedLutNodes( const void* view, bool accepted );

        /// Whether the cached LUT pipelines exist and the images could be made: the physical model's capture
        /// and sky need them.
        [[nodiscard]] bool CanBakeLuts() const
        {
            return m_TransmittanceLutPipeline && m_MultiScatterLutPipeline && !m_LutResourcesFailed;
        }
        /// The pair holds this medium: baked by an accepted graph.
        [[nodiscard]] bool LutsValid() const
        {
            return m_LutsValid;
        }
        [[nodiscard]] uint64_t GetLutBakeGeneration() const
        {
            return m_LutBakeGeneration;
        }
        [[nodiscard]] const std::shared_ptr<Image2D>& GetTransmittanceLut() const
        {
            return m_TransmittanceLut;
        }
        [[nodiscard]] const std::shared_ptr<Image2D>& GetMultiScatterLut() const
        {
            return m_MultiScatterLut;
        }
        /// The distant sky light once a node writing it was accepted (or is in the claiming graph being built);
        /// null before.
        [[nodiscard]] std::shared_ptr<Image2D> GetDistantSkyLight() const
        {
            if ( m_DistantLightWritten || m_DistantLightPending )
                return m_DistantLight;
            return nullptr;
        }
        /// The atmosphere's own sky block -- the capture's view, see the class note.
        [[nodiscard]] ShaderResources::StorageBuffer* GetSkyBlock() const
        {
            return m_SkyBlock.get();
        }

        /// The per-view fills' programs (each view keeps its own images: they follow its camera).
        [[nodiscard]] ComputePipeline* GetSkyViewLutPipeline() const
        {
            return m_SkyViewLutPipeline.get();
        }
        [[nodiscard]] ComputePipeline* GetAerialPerspectivePipeline() const
        {
            return m_AerialPerspectivePipeline.get();
        }

        static constexpr uint32_t kTransmittanceLutWidth  = 256;
        static constexpr uint32_t kTransmittanceLutHeight = 64;
        static constexpr uint32_t kMultiScatterLutSize    = 32;

    private:
        bool EnsureLutResources();
        bool EnsureDistantLightResources();
        // Each node's binding block 0: @p pipeline's shader layout (kept in @p layout), the atmosphere's block on
        // the pipeline route and the push block its exec gives.
        RenderPassDeclaration::BlockDeclaration DeclareLutBlock( RenderPassDeclaration&    declared,
                                                                 ComputePipeline*          pipeline,
                                                                 ShaderBindingLayoutCache& layout );
        [[nodiscard]] Common::BoolResultStr     DispatchTransmittanceLut( const RDG::PassContext& context );
        [[nodiscard]] Common::BoolResultStr     DispatchMultiScatterLut( const RDG::PassContext& context );
        [[nodiscard]] Common::BoolResultStr     DispatchDistantLight( const RDG::PassContext& context );

        bool m_ProgramsMade   = false;
        bool m_ProgramsFailed = false;

        std::shared_ptr<ComputePipeline> m_TransmittanceLutPipeline;
        std::shared_ptr<ComputePipeline> m_MultiScatterLutPipeline;
        std::shared_ptr<ComputePipeline> m_SkyViewLutPipeline;
        std::shared_ptr<ComputePipeline> m_AerialPerspectivePipeline;
        std::shared_ptr<ComputePipeline> m_DistantLightPipeline;
        ShaderBindingLayoutCache         m_TransmittanceLutLayout;
        ShaderBindingLayoutCache         m_MultiScatterLutLayout;
        ShaderBindingLayoutCache         m_DistantLightLayout;

        // NON-PERSISTENT: one copy per (frame in flight x renderer slot); written by the claiming view inside its
        // scope and bound only by the nodes that view's graph records.
        std::shared_ptr<ShaderResources::StorageBuffer> m_SkyBlock;
        // What UploadSkyBlock sent this frame (SkyBlockLutInputsOf); empty when the upload was refused.
        std::optional<uint64_t> m_UploadedBlockInputs;

        std::shared_ptr<Image2D> m_TransmittanceLut;
        std::shared_ptr<Image2D> m_MultiScatterLut;
        // ONE RGBA32F texel: the full-sphere mean the height fog reads (Programs/Sky/SkyDistantLight.shader).
        std::shared_ptr<Image2D> m_DistantLight;

        AtmosphereLutFingerprint                m_LutBaked;
        std::optional<AtmosphereLutFingerprint> m_LutBakePending;
        bool                                    m_LutsValid          = false;
        bool                                    m_LutResourcesFailed = false;
        // Accepted bakes of the cached pair: a dependant filled against an older pair is stale.
        uint64_t m_LutBakeGeneration = 0;

        std::optional<uint64_t> m_DistantLightInputs;
        std::optional<uint64_t> m_DistantLightInputsPending;
        bool                    m_DistantLightPending         = false; // its node is in the claiming graph
        bool                    m_DistantLightWritten         = false; // an accepted graph wrote it
        bool                    m_DistantLightResourcesFailed = false;

        // The frame's claim: the view that uploads and records, and the frame it claimed.
        const void* m_Claimant   = nullptr;
        uint64_t    m_ClaimFrame = ~uint64_t{ 0 };
    };
} // namespace Desert::Graphic
