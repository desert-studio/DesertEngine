#pragma once

#include <Engine/Generated/ShaderPushConstants.gen.hpp>
#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Texture.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/PostProcessing/BloomGaussianRules.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/Systems/Scene/PostProcessing/TonemapRenderer.hpp>
#include <Engine/Assets/Common.hpp>

#include <string>
#include <string_view>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>

namespace Desert::Graphic::System
{
    // UE 5.8's bloom, both methods (EBloomMethod), as compute nodes of each frame's graph
    // (SceneRendererFramePostFX.cpp "PostFX: Bloom*"), every one declaring what it samples and writes:
    //   * BloomSetup: the half-resolution input both methods start from - scene colour through a 4x4 box, made
    //     finite, locally exposed, and UE's threshold law on the exposed luminance (PostProcessBloomSetup.usf).
    //   * Gaussian (BM_SOG): BloomDownsample walks 1/2 -> 1/64 (UE's scene downsample chain), then each of the
    //     six stages, coarsest first, is blurred horizontally and vertically with its own Gaussian
    //     (BloomGaussianRules.hpp) and summed with its tint onto the coarser stage's result
    //     (PostProcessBloomSetup.cpp AddGaussianBloomPasses). The last vertical pass is the bloom.
    //   * Convolution (BM_FFT): the setup texture is convolved with the view's kernel in place.
    // The tonemap adds the result in unscaled - the intensity is inside the stage tints or the convolution.
    struct BloomSetupShaderParameters
    {
        DESERT_SHADER_PARAMETER( Texture, SceneColorTexture );
        DESERT_SHADER_PARAMETER( Sampler, LinearClampSampler );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        DESERT_SHADER_PARAMETER( Texture, AdaptedLuminanceTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, BloomSetupTexture );
        DESERT_SHADER_PARAMETER( Texture, LumBilateralGrid );
        DESERT_SHADER_PARAMETER( Texture, BlurredLogLuminance );
        static constexpr std::array kAll{ RDG::Erase( SceneColorTexture ),  RDG::Erase( LinearClampSampler ),
                                          RDG::Erase( PointClampSampler ),  RDG::Erase( AdaptedLuminanceTexture ),
                                          RDG::Erase( BloomSetupTexture ),  RDG::Erase( LumBilateralGrid ),
                                          RDG::Erase( BlurredLogLuminance ) };
    };

    // The Downsample shader's parameters (shared with BackdropBlurRenderer, which spells its own).
    struct BloomDownsampleShaderParameters
    {
        DESERT_SHADER_PARAMETER( Texture, SourceTexture );
        DESERT_SHADER_PARAMETER( Sampler, LinearClampSampler );
        DESERT_SHADER_PARAMETER( StorageTexture, DestinationTexture );
        static constexpr std::array kAll{ RDG::Erase( SourceTexture ), RDG::Erase( LinearClampSampler ),
                                          RDG::Erase( DestinationTexture ) };
    };

    // The Gaussian blur's parameters: one axis of one stage, the stage's kernel in a storage buffer.
    struct BloomGaussianBlurShaderParameters
    {
        DESERT_SHADER_PARAMETER( Texture, SourceTexture );
        DESERT_SHADER_PARAMETER( Texture, AdditiveTexture );
        DESERT_SHADER_PARAMETER( Sampler, LinearClampSampler );
        DESERT_SHADER_PARAMETER( StorageTexture, DestinationTexture );
        DESERT_SHADER_PARAMETER( StorageBuffer, KernelBuffer );
        static constexpr std::array kAll{ RDG::Erase( SourceTexture ), RDG::Erase( AdditiveTexture ),
                                          RDG::Erase( LinearClampSampler ), RDG::Erase( DestinationTexture ),
                                          RDG::Erase( KernelBuffer ) };
    };

    // What one frame's bloom is made of, resolved from the view's PostProcessSettings and the tier
    // (SceneRenderer::BeginScene). The renderer keeps nothing else between frames but pipelines and the
    // kernel spectrum.
    struct BloomFrameParams
    {
        bool              On        = false; // BloomGaussian::BloomOn: the method's gate; off adds no node
        Core::BloomMethod Method    = Core::BloomMethod::Gaussian;
        float             Threshold = -1.0f; // UE BloomThreshold, exposed luminance; <= -1 keeps every pixel
        // PostProcess.BloomQuality: how many of the six stages run, coarsest first (UE drops Bloom1 first).
        uint32_t                                                     StageCount = BloomGaussian::kStageCount;
        std::array<BloomGaussian::Stage, BloomGaussian::kStageCount> Stages{}; // BloomGaussian::Stages
        float ConvolutionIntensity = 0.0f; // BloomIntensity x BloomConvolutionIntensity
        bool  AutoExposure         = false;
        float ExposureKey          = 0.18f;
    };

    // Convolution bloom (UE PostProcessFFTBloom): the five programs' parameters, one spelling each.
    struct BloomConvolutionPackShaderParameters
    {
        DESERT_SHADER_PARAMETER( Texture, BloomInput );
        DESERT_SHADER_PARAMETER( Sampler, LinearClampSampler );
        DESERT_SHADER_PARAMETER( StorageTexture, Spectrum );
        static constexpr std::array kAll{ RDG::Erase( BloomInput ), RDG::Erase( LinearClampSampler ), RDG::Erase( Spectrum ) };
    };
    struct BloomConvolutionKernelShaderParameters
    {
        DESERT_SHADER_PARAMETER( Texture, KernelImage );
        DESERT_SHADER_PARAMETER( Sampler, LinearClampSampler );
        DESERT_SHADER_PARAMETER( StorageTexture, Spectrum );
        static constexpr std::array kAll{ RDG::Erase( KernelImage ), RDG::Erase( LinearClampSampler ), RDG::Erase( Spectrum ) };
    };
    struct BloomFFTShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageTexture, Spectrum );
        static constexpr std::array kAll{ RDG::Erase( Spectrum ) };
    };
    struct BloomConvolveShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageTexture, Spectrum );
        DESERT_SHADER_PARAMETER( StorageTexture, KernelSpectrum );
        static constexpr std::array kAll{ RDG::Erase( Spectrum ), RDG::Erase( KernelSpectrum ) };
    };
    struct BloomConvolutionUnpackShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageTexture, Spectrum );
        DESERT_SHADER_PARAMETER( StorageTexture, BloomOutput );
        static constexpr std::array kAll{ RDG::Erase( Spectrum ), RDG::Erase( BloomOutput ) };
    };

    class BloomRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        virtual Common::BoolResultStr Initialize() override;

        // This frame's bloom (BeginScene), read by every Declare/Record below and by PrepareConvolution.
        void SetFrameParams( const BloomFrameParams& params )
        {
            m_Frame = params;
        }
        [[nodiscard]] const BloomFrameParams& GetFrameParams() const
        {
            return m_Frame;
        }
        // The scale the scene colour is stored at (ViewFrame::PreExposure), known once the frame is: the setup
        // undoes it for the threshold's exposed luminance and keeps its output in the same stored scale.
        void SetPreExposure( float preExposure )
        {
            m_PreExposure = preExposure;
        }
        // This frame's local exposure (UE r.Bloom.ApplyLocalExposure): the setup multiplies each tap by it, with
        // the manual @p exposure the composite uses when auto-exposure is off.
        void SetLocalExposure( const TonemapRenderer::LocalExposureFrame& frame, float exposure )
        {
            m_LocalExposure  = frame;
            m_ManualExposure = exposure;
        }

        // The setup texture of this frame: half the scene colour, one mip - stage 0 of the Gaussian chain and the
        // convolution's input and output. Nullopt when there is nothing to record (no scene colour or pipelines).
        // A transient of the frame graph (Builder::CreateTexture); the renderer keeps no image.
        [[nodiscard]] std::optional<RDG::TextureDesc> GetSetupDesc() const;
        // Stage @p stage (0 = the setup's 1/2 .. 5 = 1/64 of the view) of the chain the setup starts.
        [[nodiscard]] static RDG::TextureDesc GetStageDesc( const RDG::TextureDesc& setupDesc, uint32_t stage );

        // Setup: @p sceneColor -> @p setup. @p adaptedLuminance is this frame's auto-exposure adapted luminance
        // (1x1), or the system white when there is none. @p localExposureGrid / @p blurredLogLuminance:
        // FrameTransients' when the local exposure ran, else System.Black3D / System.Black.
        void DeclareSetupBindings( RDG::PassBuilder& pass, RDG::TextureRef sceneColor, RDG::TextureRef setup,
                                   RDG::TextureRef adaptedLuminance, RDG::TextureRef localExposureGrid,
                                   RDG::TextureRef blurredLogLuminance ) const;
        [[nodiscard]] Common::BoolResultStr RecordSetup( const RDG::PassContext& context,
                                                         const RDG::TextureDesc& setupDesc );
        // One step of the chain: @p source (stage k - 1) -> @p destination (stage k), a 4x4 box.
        void DeclareDownsampleBindings( RDG::PassBuilder& pass, RDG::TextureRef source,
                                        RDG::TextureRef destination ) const;
        [[nodiscard]] Common::BoolResultStr RecordDownsample( const RDG::PassContext& context,
                                                              const RDG::TextureDesc& sourceDesc,
                                                              const RDG::TextureDesc& destinationDesc ) const;
        // One axis of one Gaussian stage: @p source blurred with @p kernel into @p destination, x the push's tint,
        // plus @p additive (the coarser stage's result; System.Black on a horizontal pass or the coarsest stage).
        void DeclareBlurBindings( RDG::PassBuilder& pass, RDG::TextureRef source, RDG::TextureRef additive,
                                  RDG::TextureRef destination, RDG::BufferRef kernel ) const;
        [[nodiscard]] Common::BoolResultStr RecordBlur( const RDG::PassContext&              context,
                                                        const ShaderPush::BloomGaussianBlur& push,
                                                        const RDG::TextureDesc& destinationDesc ) const;
        // The push of one blur pass of the stage whose size is @p stageDesc: horizontal (@p vertical false) with a
        // white tint and no additive, vertical with the stage's @p tint and @p additive.
        [[nodiscard]] static ShaderPush::BloomGaussianBlur MakeBlurPush( const RDG::TextureDesc&      stageDesc,
                                                                         const BloomGaussian::Kernel& kernel,
                                                                         bool vertical, const glm::vec3& tint,
                                                                         bool additive );
        // The kernel of Gaussian stage @p stage (0 = 1/2 .. 5 = 1/64) for a stage @p stageWidth texels wide and a
        // width of @p kernelSizePercent of it, with its storage buffer in @p graph. Kept between frames (UE caches
        // the blur kernel per pass): computed and uploaded only when the stage's (width, size percent) changes,
        // else the kept buffer is registered as is.
        [[nodiscard]] const BloomGaussian::Kernel& AcquireKernel( RDG::Builder& graph, uint32_t stage,
                                                                  uint32_t stageWidth, float kernelSizePercent,
                                                                  RDG::BufferRef& buffer );

        // CONVOLUTION BLOOM (UE BM_FFT, PostProcessFFTBloom): the method is the view's (BloomFrameParams::Method),
        // the kernel the view's (PostProcessSettings::BloomConvolutionKernel / Scale).
        void SetConvolutionKernel( Assets::AssetHandle kernel, float scale )
        {
            m_Kernel      = kernel;
            m_KernelScale = std::clamp( scale, 0.0f, 1.0f );
        }
        [[nodiscard]] Assets::AssetHandle GetConvolutionKernel() const
        {
            return m_Kernel;
        }

        // This frame's convolution: the FFT size (BloomConvolutionFFTSize, BloomConvolutionRules.hpp - the size
        // the view's memory census lists too), the content rectangle the half-resolution setup texture is
        // resampled onto (the rest is the zero border, UE BloomConvolutionBufferScale), and whether the kernel's
        // spectrum must be rebuilt this frame.
        struct ConvolutionPlan
        {
            std::string      KernelName;     // the kernel's source path, for the outcome line
            uint32_t  Width      = 0; // N.x
            uint32_t  Height     = 0; // N.y
            uint32_t  Log2Width  = 0;
            uint32_t  Log2Height = 0;
            glm::vec2 Content{ 0.0f };
            glm::vec2 KernelSize{ 0.0f }; // the kernel image's footprint, spectrum texels
            float     KernelLod     = 0.0f;
            glm::uvec2       KernelResident{ 0u };    // the kernel image's resident top level, texels
            const Texture2D* KernelImage   = nullptr; // the kernel the spectrum is (re)built from
            bool             RebuildKernel = false;
        };
        // Nullopt = this frame runs the Gaussian chain: the view asks for Gaussian, or it names no kernel, or its
        // image is still loading (@p kernel null), or a pipeline is missing. The frame's outcome is logged
        // once per change, never per frame (UE FFTBloom's kernel errors are warn-once): the Gaussian line with its
        // reason here, "[Bloom] method=Convolution kernel=<name> fft=<N.x>x<N.y>" by AddConvolutionPasses once the
        // passes are recorded, or DropConvolution's line when the caller cannot run a plan it was given. Allocates
        // (or keeps) the kernel spectrum image for the plan's size.
        [[nodiscard]] std::optional<ConvolutionPlan> PrepareConvolution( const RDG::TextureDesc& setupDesc,
                                                                         const Texture2D*        kernel,
                                                                         const std::string&      kernelName );
        // The kernel's 2D spectrum, kept between frames (rebuilt when ConvolutionPlan::RebuildKernel).
        [[nodiscard]] const std::shared_ptr<Image2D>& GetKernelSpectrum() const
        {
            return m_KernelSpectrum;
        }
        // Records the convolution into @p graph: (kernel: place + row FFT + column FFT into @p kernelSpectrum when
        // the plan says so), then pack @p setup, row FFT, column FFT x kernel x inverse column FFT, inverse row
        // FFT, unpack x ConvolutionIntensity back into @p setup - the texture the composite adds in.
        void AddConvolutionPasses( RDG::Builder& graph, const ConvolutionPlan& plan, RDG::TextureRef setup,
                                   const RDG::TextureDesc& setupDesc, RDG::TextureRef kernelImage,
                                   RDG::TextureRef kernelSpectrum );
        // A planned convolution the frame cannot run (its kernel image or spectrum did not import): this frame
        // runs the Gaussian chain, and "[Bloom] method=Gaussian (view Convolution: <reason>)" is warned once per
        // reason - the same line every frame the cause persists, so it is logged on its first frame only.
        void DropConvolution( std::string_view reason );

    private:
        bool CreatePipelines();
        // Logs @p line when it differs from the last outcome logged (m_LoggedOutcome); a warning for a failure.
        void ReportOutcome( std::string line, bool warning );

        std::shared_ptr<ComputePipeline> m_SetupPipeline;
        std::shared_ptr<ComputePipeline> m_DownsamplePipeline;
        std::shared_ptr<ComputePipeline> m_BlurPipeline;
        mutable ShaderBindingLayoutCache m_SetupLayout; // the three shaders' layouts, kept between frames
        mutable ShaderBindingLayoutCache m_DownsampleLayout;
        mutable ShaderBindingLayoutCache m_BlurLayout;

        BloomFrameParams                    m_Frame;
        float                               m_PreExposure = 1.0f;
        TonemapRenderer::LocalExposureFrame m_LocalExposure;
        float                               m_ManualExposure = 1.0f;

        // One Gaussian stage's kernel and the buffer it was uploaded to, with what it was computed from.
        struct StageKernel
        {
            uint32_t              StageWidth  = 0;
            float                 SizePercent = -1.0f;
            BloomGaussian::Kernel Kernel;
            RDG::ExternalBuffer   Buffer;
        };
        std::array<StageKernel, BloomGaussian::kStageCount> m_StageKernels;

        struct ConvolutionProgram
        {
            std::shared_ptr<ComputePipeline> Pipeline;
            mutable ShaderBindingLayoutCache Layout;
        };
        ConvolutionProgram m_Pack;
        ConvolutionProgram m_KernelPlace;
        ConvolutionProgram m_FFT;
        ConvolutionProgram m_Convolve;
        ConvolutionProgram m_Unpack;
        bool               m_ConvolutionReady = false; // all five programs built and validated

        Assets::AssetHandle              m_Kernel;
        float                            m_KernelScale = 1.0f;
        std::shared_ptr<Image2D>         m_KernelSpectrum;
        // What the kernel spectrum was built from: a change of any rebuilds it.
        const Texture2D* m_BuiltKernel = nullptr;
        float            m_BuiltScale  = -1.0f;
        glm::vec2        m_BuiltContent{ 0.0f };
        glm::uvec2       m_BuiltResident{ 0u };
        std::string      m_LoggedOutcome; // the last "[Bloom] method=..." line, logged on change only
    };
} // namespace Desert::Graphic::System
