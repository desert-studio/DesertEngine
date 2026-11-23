#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Texture.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/Systems/Scene/PostProcessing/TonemapRenderer.hpp>
#include <Engine/Assets/Common.hpp>
#include <Common/Settings/CapabilityCatalog.hpp>

#include <string>
#include <string_view>

#include <algorithm>
#include <memory>
#include <optional>

namespace Desert::Graphic::System
{
    // Bloom via a compute mip-chain (Call of Duty / Jimenez): progressive 13-tap downsample of the HDR
    // scene color (with a Karis average + bright-pass on the first mip), then a tent-filtered additive
    // upsample back to mip 0. The chain is a transient of each frame's graph (FrameTextures::Transients.Bloom),
    // whose mip 0 the tonemap adds in. Every dispatch is its own compute node (SceneRendererFramePostFX.cpp
    // "PostFX: Bloom*"), which declares the mip it samples and the mip it writes and places every barrier.
    // The Downsample shader's parameters: the one spelling the pass binds and Initialize checks.
    struct BloomDownsampleShaderParameters
    {
        DESERT_SHADER_PARAMETER( Texture, SourceMipTexture );
        DESERT_SHADER_PARAMETER( Sampler, LinearClampSampler );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        DESERT_SHADER_PARAMETER( Texture, AdaptedLuminanceTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, DestinationMipTexture );
        DESERT_SHADER_PARAMETER( Texture, LumBilateralGrid );
        DESERT_SHADER_PARAMETER( Texture, BlurredLogLuminance );
        static constexpr std::array kAll{
             RDG::Erase( SourceMipTexture ),      RDG::Erase( LinearClampSampler ),
             RDG::Erase( PointClampSampler ),     RDG::Erase( AdaptedLuminanceTexture ),
             RDG::Erase( DestinationMipTexture ), RDG::Erase( LumBilateralGrid ),
             RDG::Erase( BlurredLogLuminance ) };
    };

    // The Upsample shader's parameters: the one spelling the pass binds and Initialize checks.
    struct BloomUpsampleShaderParameters
    {
        DESERT_SHADER_PARAMETER( Texture, SourceMipTexture );
        DESERT_SHADER_PARAMETER( Sampler, LinearClampSampler );
        DESERT_SHADER_PARAMETER( StorageTexture, DestinationMipTexture );
        static constexpr std::array kAll{ RDG::Erase( SourceMipTexture ), RDG::Erase( LinearClampSampler ), RDG::Erase( DestinationMipTexture ) };
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

        // The chain this frame needs, from this frame's scene colour size: half resolution, mips capped so the
        // smallest stays a sane size. Nullopt when there is nothing to record (no scene colour or pipelines).
        // The texture is a transient of the frame graph (Builder::CreateTexture); the renderer keeps no image.
        [[nodiscard]] std::optional<RDG::TextureDesc> GetChainDesc() const;
        // Downsample into @p mip of @p chain: mip 0 samples @p sceneColor (Karis + threshold), mip i samples
        // mip i-1, both LinearClamp. SETUP declares that block (block 0); the EXEC dispatches from it.
        // @p adaptedLuminance is this frame's auto-exposure adapted luminance (1x1), or the system white when
        // there is none: with auto-exposure on, the threshold is stated after exposure, as it is in manual.
        // @p localExposureGrid / @p blurredLogLuminance: FrameTransients' when the local exposure ran, else
        // System.Black3D / System.Black (SetLocalExposure's frame then says not produced).
        void DeclareDownsampleBindings( RDG::PassBuilder& pass, RDG::TextureRef sceneColor, RDG::TextureRef chain,
                                        RDG::TextureRef adaptedLuminance, RDG::TextureRef localExposureGrid,
                                        RDG::TextureRef blurredLogLuminance, uint32_t mip ) const;
        [[nodiscard]] Common::BoolResultStr RecordDownsample( const RDG::PassContext& context,
                                                              const RDG::TextureDesc& chainDesc, uint32_t mip );
        // Additive upsample: samples @p mip (>= 1) of @p chain (LinearClamp) and accumulates into mip - 1.
        void DeclareUpsampleBindings( RDG::PassBuilder& pass, RDG::TextureRef chain, uint32_t mip ) const;
        [[nodiscard]] Common::BoolResultStr RecordUpsample( const RDG::PassContext& context,
                                                            const RDG::TextureDesc& chainDesc, uint32_t mip );

        void SetThreshold( float threshold )
        {
            m_Threshold = threshold;
        }
        // The scale the scene colour is stored at (ViewFrame::PreExposure): the bright pass compares the STORED
        // scene, so the threshold (raw radiance, or exposed units under auto-exposure) is multiplied by it.
        void SetPreExposure( float preExposure )
        {
            m_PreExposure = preExposure;
        }
        // Auto-exposure on: the threshold is the authored one (exposed units) and the shader divides it by
        // this frame's exposure, key / adapted luminance — the composite's own formula.
        void SetAutoExposure( bool enabled, float key )
        {
            m_AutoExposure = enabled;
            m_ExposureKey  = key;
        }
        // The chain's mip count cap, PostProcess.BloomMips (Scalability; High = 6, the former constant). Read per
        // frame by GetChainDesc, so a change applies on the next frame's graph.
        // This frame's local exposure (UE r.Bloom.ApplyLocalExposure): mip 0 multiplies each tap by it, with the
        // manual @p exposure the composite uses when auto-exposure is off.
        void SetLocalExposure( const TonemapRenderer::LocalExposureFrame& frame, float exposure )
        {
            m_LocalExposure  = frame;
            m_ManualExposure = exposure;
        }

        void SetMaxMips( uint32_t mips )
        {
            m_MaxMips = std::max( 1u, mips );
        }

        // CONVOLUTION BLOOM (UE r.Bloom.Method 1, PostProcessFFTBloom). The tier's method (Scalability
        // PostProcess.BloomMethod) and the view's kernel (PostProcessSettings::BloomConvolutionKernel / Scale).
        void SetMethod( Common::Scalability::BloomMethod method )
        {
            m_Method = method;
        }
        [[nodiscard]] Common::Scalability::BloomMethod GetMethod() const
        {
            return m_Method;
        }
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
        // the view's memory census lists too), the content rectangle the half-resolution bloom input is resampled
        // onto (the rest is the zero border, UE BloomConvolutionBufferScale), and whether the kernel's spectrum
        // must be rebuilt this frame.
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
        // Nullopt = this frame runs the Gaussian chain: the tier asks for Gaussian, or the view names no kernel,
        // or its image is still loading (@p kernel null), or a pipeline is missing. The frame's outcome is logged
        // once per change, never per frame (UE FFTBloom's kernel errors are warn-once): the Gaussian line with its
        // reason here, "[Bloom] method=Convolution kernel=<name> fft=<N.x>x<N.y>" by AddConvolutionPasses once the
        // passes are recorded, or DropConvolution's line when the caller cannot run a plan it was given. Allocates
        // (or keeps) the kernel spectrum image for the plan's size.
        [[nodiscard]] std::optional<ConvolutionPlan> PrepareConvolution( const RDG::TextureDesc& chainDesc,
                                                                         const Texture2D*        kernel,
                                                                         const std::string&      kernelName );
        // The kernel's 2D spectrum, kept between frames (rebuilt when ConvolutionPlan::RebuildKernel).
        [[nodiscard]] const std::shared_ptr<Image2D>& GetKernelSpectrum() const
        {
            return m_KernelSpectrum;
        }
        // Records the convolution into @p graph: (kernel: place + row FFT + column FFT into @p kernelSpectrum when
        // the plan says so), then pack mip 0 of @p chain, row FFT, column FFT x kernel x inverse column FFT,
        // inverse row FFT, unpack into mip 0 of @p chain — the texture the composite adds in for either method.
        void AddConvolutionPasses( RDG::Builder& graph, const ConvolutionPlan& plan, RDG::TextureRef chain,
                                   RDG::TextureRef kernelImage, RDG::TextureRef kernelSpectrum );
        // A planned convolution the frame cannot run (its kernel image or spectrum did not import): this frame
        // runs the Gaussian chain, and "[Bloom] method=Gaussian (tier Convolution: <reason>)" is warned once per
        // reason - the same line every frame the cause persists, so it is logged on its first frame only.
        void DropConvolution( std::string_view reason );

    private:
        bool CreatePipelines();
        // Logs @p line when it differs from the last outcome logged (m_LoggedOutcome); a warning for a failure.
        void ReportOutcome( std::string line, bool warning );

        static constexpr float    kFilterRadius = 1.0f; // tent radius (source texels) for upsampling

        std::shared_ptr<ComputePipeline>  m_DownsamplePipeline;
        std::shared_ptr<ComputePipeline>  m_UpsamplePipeline;
        mutable ShaderBindingLayoutCache  m_DownsampleLayout; // the two shaders' layouts, kept between frames
        mutable ShaderBindingLayoutCache  m_UpsampleLayout;

        float    m_Threshold = 1.0f;
        float    m_PreExposure  = 1.0f;
        bool     m_AutoExposure = false;
        float    m_ExposureKey  = 0.18f;
        TonemapRenderer::LocalExposureFrame m_LocalExposure;
        float                               m_ManualExposure = 1.0f;
        uint32_t m_MaxMips   = 6; // until the first SetMaxMips: the High table value

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

        Common::Scalability::BloomMethod m_Method = Common::Scalability::BloomMethod::Gaussian;
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
