#include <Engine/Graphic/Upscalers/FSR/Fsr3Upscaler.hpp>

#include <Engine/Core/EngineContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanAllocator.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRenderGraph.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>

#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>

#include <array>
#include <cfloat>
#include <cmath>
#include <format>
#include <string>
#include <vector>

namespace Desert::Graphic
{
    namespace
    {
        constexpr float kCentimetresToMetres = 0.01f; // 1 engine unit = 1 cm (viewSpaceToMetersFactor)

        // FSR's debug messages (FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING, Debug builds) are ASCII in wide strings.
        void ForwardFfxMessage( const FfxMsgType type, const wchar_t* message )
        {
            std::string text;
            for ( const wchar_t* c = message; c != nullptr && *c != L'\0'; ++c )
                text.push_back( *c < 0x80 ? static_cast<char>( *c ) : '?' );
            if ( type == FFX_MESSAGE_TYPE_ERROR )
            {
                LOG_ERROR( "[FSR3] {}", text );
            }
            else
            {
                LOG_WARN( "[FSR3] {}", text );
            }
        }

        // What a context is created for. A frame whose key differs gets a new context (the old one is retired).
        struct ContextKey
        {
            ViewExtent Render;
            ViewExtent Output;
            uint32_t   Flags = 0;

            bool operator==( const ContextKey& ) const = default;
        };

        uint32_t ContextFlags( const ViewFrame& frame )
        {
            uint32_t flags = FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE | FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED;
            if ( std::isinf( frame.FarPlane ) )
                flags |= FFX_FSR3UPSCALER_ENABLE_DEPTH_INFINITE;
#if defined( DESERT_CONFIG_DEBUG )
            flags |= FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING;
#endif
            return flags;
        }

        // Everything one view's FSR owns. Heap-held and shared with the retirement callback: the context holds
        // pointers into itself (the backend interface, the device context).
        struct Fsr3State
        {
            ContextKey             Key;
            VkDeviceContext        Device{};
            std::vector<std::byte> Scratch;
            FfxInterface           Interface{};
            FfxFsr3UpscalerContext Context{};
            bool                   ContextCreated = false;
            std::array<FfxResourceInternal, 3>
                     Shared{}; // dilated depth, dilated motion vectors, prev nearest depth
            uint32_t          SharedCreated = 0;
            ResourceOwnership Memory; // the context's device memory as FSR reports it

            void Destroy()
            {
                for ( uint32_t i = 0; i < SharedCreated; ++i )
                    Interface.fpDestroyResource( &Interface, Shared[i], 0 );
                SharedCreated = 0;
                if ( ContextCreated )
                    ffxFsr3UpscalerContextDestroy( &Context );
                ContextCreated = false;
                Memory         = ResourceOwnership{};
            }
        };

        class Fsr3History final : public ITemporalUpscalerHistory
        {
        public:
            explicit Fsr3History( std::shared_ptr<Fsr3State> state ) : m_State( std::move( state ) )
            {
            }

            ~Fsr3History() override
            {
                // A frame in flight may still execute this context's dispatch: retire it with the frame ring.
                const auto ctx = EngineContext::GetInstance().GetRendererContext();
                auto*      allocator =
                     ctx ? SP_CAST( API::Vulkan::VulkanContext, ctx )->GetVulkanAllocator().get() : nullptr;
                if ( allocator != nullptr )
                    allocator->RT_DestroyLater( [state = m_State]() { state->Destroy(); } );
                else
                    m_State->Destroy(); // no renderer: no frame in flight records with it
            }

            [[nodiscard]] std::string_view DebugName() const override
            {
                return "FSR3";
            }

            [[nodiscard]] const std::shared_ptr<Fsr3State>& State() const
            {
                return m_State;
            }

        private:
            std::shared_ptr<Fsr3State> m_State;
        };

        std::string FfxErrorName( const FfxErrorCode code )
        {
            return std::format( "FfxErrorCode {:#x}", static_cast<uint32_t>( code ) );
        }

        Common::ResultStr<std::shared_ptr<Fsr3State>> CreateState( const ContextKey& key )
        {
            const auto device =
                 SP_CAST( API::Vulkan::VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() );
            if ( !device )
                return Common::MakeFormattedError<std::shared_ptr<Fsr3State>>(
                     "{}", "FSR3: the engine device is not a Vulkan device" );

            auto state                     = std::make_shared<Fsr3State>();
            state->Key                     = key;
            state->Device.vkDevice         = device->GetVulkanLogicalDevice();
            state->Device.vkPhysicalDevice = device->GetPhysicalDevice()->GetVulkanPhysicalDevice();
            state->Device.vkDeviceProcAddr = vkGetDeviceProcAddr;

            // One effect context per backend interface: this view's upscaler.
            constexpr size_t kContexts = 1;
            state->Scratch.resize( ffxGetScratchMemorySizeVK( state->Device.vkPhysicalDevice, kContexts ) );
            if ( const FfxErrorCode error =
                      ffxGetInterfaceVK( &state->Interface, ffxGetDeviceVK( &state->Device ),
                                         state->Scratch.data(), state->Scratch.size(), kContexts );
                 error != FFX_OK )
                return Common::MakeFormattedError<std::shared_ptr<Fsr3State>>(
                     "FSR3: ffxGetInterfaceVK failed ({})", FfxErrorName( error ) );

            FfxFsr3UpscalerContextDescription description{};
            description.flags            = key.Flags;
            description.maxRenderSize    = { key.Render.Width, key.Render.Height };
            description.maxUpscaleSize   = { key.Output.Width, key.Output.Height };
            description.fpMessage        = &ForwardFfxMessage;
            description.backendInterface = state->Interface;
            if ( const FfxErrorCode error = ffxFsr3UpscalerContextCreate( &state->Context, &description );
                 error != FFX_OK )
                return Common::MakeFormattedError<std::shared_ptr<Fsr3State>>(
                     "FSR3: ffxFsr3UpscalerContextCreate for {}x{} -> {}x{} failed ({})", key.Render.Width,
                     key.Render.Height, key.Output.Width, key.Output.Height, FfxErrorName( error ) );
            state->ContextCreated = true;

            // FSR 3.1 takes three intermediates from the caller (its frame interpolation would share them); the
            // upscaler alone keeps them with its context, created by the backend as ffx-api's provider does.
            FfxFsr3UpscalerSharedResourceDescriptions shared{};
            if ( const FfxErrorCode error =
                      ffxFsr3UpscalerGetSharedResourceDescriptions( &state->Context, &shared );
                 error != FFX_OK )
            {
                state->Destroy();
                return Common::MakeFormattedError<std::shared_ptr<Fsr3State>>(
                     "FSR3: ffxFsr3UpscalerGetSharedResourceDescriptions failed ({})", FfxErrorName( error ) );
            }
            const std::array<FfxCreateResourceDescription*, 3> descriptions{
                 &shared.dilatedDepth, &shared.dilatedMotionVectors, &shared.reconstructedPrevNearestDepth };
            for ( FfxCreateResourceDescription* resource : descriptions )
            {
                if ( const FfxErrorCode error = state->Interface.fpCreateResource(
                          &state->Interface, resource, 0, &state->Shared[state->SharedCreated] );
                     error != FFX_OK )
                {
                    state->Destroy();
                    return Common::MakeFormattedError<std::shared_ptr<Fsr3State>>(
                         "FSR3: creating the shared resource {} failed ({})", state->SharedCreated,
                         FfxErrorName( error ) );
                }
                ++state->SharedCreated;
            }

            FfxEffectMemoryUsage usage{};
            if ( ffxFsr3UpscalerContextGetGpuMemoryUsage( &state->Context, &usage ) == FFX_OK )
                state->Memory = ResourceOwnership::Take( ResourceKind::VendorUpscaler, usage.totalUsageInBytes );
            return Common::MakeSuccess( std::move( state ) );
        }

        // An engine graph texture as a FidelityFX resource, in the state the graph left it in for this pass
        // (sampled in compute: SHADER_READ_ONLY_OPTIMAL = FFX_RESOURCE_STATE_COMPUTE_READ; storage write:
        // GENERAL = FFX_RESOURCE_STATE_UNORDERED_ACCESS). The backend returns every external resource to that
        // state at the end of its dispatch, so the graph's tracking stays true.
        Common::ResultStr<FfxResource> ResourceOf( const RDG::PassContext& context, const RDG::TextureRef texture,
                                                   const RDG::Access access, const ViewExtent extent,
                                                   const wchar_t* name )
        {
            const auto binding = context.GetTexture( texture, access );
            if ( !binding )
                return Common::MakeFormattedError<FfxResource>( "FSR3: {}", binding.GetError() );
            const auto physical = API::Vulkan::VulkanRdgBackend::TextureOf( binding.GetValue() );
            if ( !physical )
                return Common::MakeFormattedError<FfxResource>( "FSR3: {}", physical.GetError() );

            const VkFormat         format = physical.GetValue()->GetFormat();
            const bool             write  = access == RDG::Access::StorageWrite;
            FfxResourceDescription description{};
            description.type     = FFX_RESOURCE_TYPE_TEXTURE2D;
            description.format   = ffxGetSurfaceFormatVK( format );
            description.width    = extent.Width;
            description.height   = extent.Height;
            description.depth    = 1;
            description.mipCount = 1;
            description.flags    = FFX_RESOURCE_FLAGS_NONE;
            description.usage    = format == VK_FORMAT_D32_SFLOAT ? FFX_RESOURCE_USAGE_DEPTHTARGET
                                   : write                        ? FFX_RESOURCE_USAGE_UAV
                                                                  : FFX_RESOURCE_USAGE_READ_ONLY;
            return Common::MakeSuccess( ffxGetResourceVK( physical.GetValue()->GetImage(), description, name,
                                                          write ? FFX_RESOURCE_STATE_UNORDERED_ACCESS
                                                                : FFX_RESOURCE_STATE_COMPUTE_READ ) );
        }
    } // namespace

    Fsr3Upscaler::Fsr3Upscaler()  = default;
    Fsr3Upscaler::~Fsr3Upscaler() = default;

    bool Fsr3Upscaler::Supports( const ResolutionSplit& split ) const
    {
        return split.Mode != Common::Scalability::ScaleMode::Supersample;
    }

    std::vector<HistoryTextureDesc> Fsr3Upscaler::HistoryDescs( const ResolutionSplit&,
                                                                const Common::Scalability::ResolvedQuality& ) const
    {
        return {};
    }

    uint32_t Fsr3Upscaler::JitterSequenceLength( const ResolutionSplit& split ) const
    {
        if ( split.Render.Width == 0 || split.Output.Width == 0 )
            return 8; // an empty view draws nothing; FSR's native length keeps the sequence defined
        return static_cast<uint32_t>( ffxFsr3UpscalerGetJitterPhaseCount(
             static_cast<int32_t>( split.Render.Width ), static_cast<int32_t>( split.Output.Width ) ) );
    }

    glm::vec2 Fsr3Upscaler::JitterPixels( const uint32_t index, const uint32_t length ) const
    {
        float x = 0.0f;
        float y = 0.0f;
        if ( length == 0 || ffxFsr3UpscalerGetJitterOffset( &x, &y, static_cast<int32_t>( index ),
                                                            static_cast<int32_t>( length ) ) != FFX_OK )
            return glm::vec2( 0.0f );
        return { x, -y };
    }

    float Fsr3Upscaler::MaterialMipBias( const ResolutionSplit& split ) const
    {
        if ( split.Render.Width == 0 || split.Output.Width == 0 )
            return 0.0f;
        return std::log2( static_cast<float>( split.Render.Width ) / static_cast<float>( split.Output.Width ) ) -
               1.0f;
    }

    Common::ResultStr<TemporalUpscalerOutputs>
    Fsr3Upscaler::AddPasses( RDG::Builder& graph, const ViewFrame& frame,
                             const TemporalUpscalerInputs& inputs ) const
    {
        const ResolutionSplit& split = frame.Split;
        if ( !Supports( split ) )
            return Common::MakeFormattedError<TemporalUpscalerOutputs>(
                 "FSR3: the split {}x{} -> {}x{} ({} %) is a supersample; FSR resolves at or above its input size",
                 split.Render.Width, split.Render.Height, split.Output.Width, split.Output.Height,
                 split.RenderScalePercent );
        if ( !inputs.SceneColor.IsValid() || !inputs.SceneDepth.IsValid() || !inputs.Velocity.IsValid() ||
             !inputs.ResponsiveAA.IsValid() || !inputs.Exposure.IsValid() )
            return Common::MakeFormattedError<TemporalUpscalerOutputs>(
                 "FSR3: an input is not a texture of this graph (colour {}, depth {}, velocity {}, responsive {}, "
                 "exposure {})",
                 inputs.SceneColor.IsValid(), inputs.SceneDepth.IsValid(), inputs.Velocity.IsValid(),
                 inputs.ResponsiveAA.IsValid(), inputs.Exposure.IsValid() );
        if ( !inputs.History.empty() )
            return Common::MakeFormattedError<TemporalUpscalerOutputs>(
                 "FSR3: {} history textures passed; FSR declares none (its history is in its context)",
                 inputs.History.size() );

        // This view's context: the one the last frame returned when it is FSR's and still fits, else a new one.
        const ContextKey                          key{ split.Render, split.Output, ContextFlags( frame ) };
        std::shared_ptr<ITemporalUpscalerHistory> history   = inputs.PrevHistory;
        const auto*                               previous  = dynamic_cast<const Fsr3History*>( history.get() );
        bool                                      recreated = false;
        if ( previous == nullptr || !( previous->State()->Key == key ) )
        {
            auto created = CreateState( key );
            if ( !created )
                return Common::MakeFormattedError<TemporalUpscalerOutputs>( "{}", created.GetError() );
            history   = std::make_shared<Fsr3History>( std::move( created.GetValue() ) );
            recreated = true;
        }
        const std::shared_ptr<Fsr3State> state = static_cast<const Fsr3History*>( history.get() )->State();

        const Common::ResultStr<RDG::TextureRef> exposure = m_Exposure.AddPass( graph, inputs.Exposure );
        if ( !exposure )
            return Common::MakeFormattedError<TemporalUpscalerOutputs>( "FSR3: {}", exposure.GetError() );

        RDG::TextureDesc outputDesc;
        outputDesc.Size              = { split.Output.Width, split.Output.Height, 1 };
        outputDesc.Format            = Core::Formats::ImageFormat::RGBA16F;
        const RDG::TextureRef output = graph.CreateTexture( outputDesc, "FSR3.Output" );

        // Everything but the resources, decided now (the record runs later, from the graph).
        FfxFsr3UpscalerDispatchDescription dispatch{};
        // FSR's jitter (x, y) is ViewFrame::JitterPixels (x, -y) (header comment).
        dispatch.jitterOffset      = { frame.JitterPixels.x, -frame.JitterPixels.y };
        dispatch.motionVectorScale = { -0.5f * static_cast<float>( split.Render.Width ),
                                       0.5f * static_cast<float>( split.Render.Height ) };
        dispatch.renderSize        = { split.Render.Width, split.Render.Height };
        dispatch.upscaleSize       = { split.Output.Width, split.Output.Height };
        // The engine's own sharpen (Resolution.Sharpness) runs after the resolve, outside the history.
        dispatch.enableSharpening = false;
        dispatch.sharpness        = 0.0f;
        dispatch.frameTimeDelta   = frame.DeltaSeconds * 1000.0f;
        dispatch.preExposure      = 1.0f;
        dispatch.reset            = !frame.HistoryValid() || recreated;
        // Inverted depth: FSR's "near" is the plane at depth 1 — the far plane under reversed Z.
        dispatch.cameraNear              = std::isinf( frame.FarPlane ) ? FLT_MAX : frame.FarPlane;
        dispatch.cameraFar               = frame.NearPlane;
        dispatch.cameraFovAngleVertical  = 2.0f * std::atan( 1.0f / std::abs( frame.Projection[1][1] ) );
        dispatch.viewSpaceToMetersFactor = kCentimetresToMetres;
        dispatch.flags                   = 0;

        const ViewExtent             render   = split.Render;
        const ViewExtent             upscaled = split.Output;
        const ViewExtent             unit{ 1, 1 };
        const TemporalUpscalerInputs in          = inputs;
        const RDG::TextureRef        exposureRef = exposure.GetValue();
        graph.AddPass(
             "FSR3.Upscale", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Read( in.SceneColor, RDG::Access::SampledCompute );
                 pass.Read( in.SceneDepth, RDG::Access::SampledCompute );
                 pass.Read( in.Velocity, RDG::Access::SampledCompute );
                 pass.Read( in.ResponsiveAA, RDG::Access::SampledCompute );
                 pass.Read( exposureRef, RDG::Access::SampledCompute );
                 pass.Write( output, RDG::Access::StorageWrite );
             },
             [state, dispatch, in, exposureRef, output, render, upscaled,
              unit]( RDG::PassContext& context ) mutable -> Common::BoolResultStr
             {
                 const auto commandBuffer = API::Vulkan::VulkanRdgBackend::CommandBufferOf( context );
                 if ( !commandBuffer )
                     return Common::MakeError( std::format( "FSR3: {}", commandBuffer.GetError() ) );

                 struct Bound
                 {
                     FfxResource*    Target;
                     RDG::TextureRef Texture;
                     RDG::Access     Access;
                     ViewExtent      Extent;
                     const wchar_t*  Name;
                 };
                 const std::array<Bound, 6> bound{ {
                      { &dispatch.color, in.SceneColor, RDG::Access::SampledCompute, render, L"FSR3_Color" },
                      { &dispatch.depth, in.SceneDepth, RDG::Access::SampledCompute, render, L"FSR3_Depth" },
                      { &dispatch.motionVectors, in.Velocity, RDG::Access::SampledCompute, render,
                        L"FSR3_Velocity" },
                      { &dispatch.reactive, in.ResponsiveAA, RDG::Access::SampledCompute, render,
                        L"FSR3_Reactive" },
                      { &dispatch.exposure, exposureRef, RDG::Access::SampledCompute, unit, L"FSR3_Exposure" },
                      { &dispatch.output, output, RDG::Access::StorageWrite, upscaled, L"FSR3_Output" },
                 } };
                 for ( const Bound& b : bound )
                 {
                     const Common::ResultStr<FfxResource> resource =
                          ResourceOf( context, b.Texture, b.Access, b.Extent, b.Name );
                     if ( !resource )
                         return Common::MakeError( resource.GetError() );
                     *b.Target = resource.GetValue();
                 }
                 dispatch.transparencyAndComposition = ffxGetResourceVK( nullptr, {}, L"FSR3_NoComposition" );
                 dispatch.dilatedDepth = state->Interface.fpGetResource( &state->Interface, state->Shared[0] );
                 dispatch.dilatedMotionVectors =
                      state->Interface.fpGetResource( &state->Interface, state->Shared[1] );
                 dispatch.reconstructedPrevNearestDepth =
                      state->Interface.fpGetResource( &state->Interface, state->Shared[2] );
                 dispatch.commandList = ffxGetCommandListVK( commandBuffer.GetValue() );

                 if ( const FfxErrorCode error = ffxFsr3UpscalerContextDispatch( &state->Context, &dispatch );
                      error != FFX_OK )
                     return Common::MakeError( std::format( "FSR3: ffxFsr3UpscalerContextDispatch failed ({})",
                                                            FfxErrorName( error ) ) );
                 return Common::MakeSuccess( true );
             } );

        TemporalUpscalerOutputs outputs;
        outputs.SceneColor = output;
        outputs.NewHistory = std::move( history );
        return Common::MakeSuccess( std::move( outputs ) );
    }
} // namespace Desert::Graphic
