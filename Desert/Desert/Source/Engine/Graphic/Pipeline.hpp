#pragma once

#include <Engine/Graphic/PipelineBuilds.hpp>
#include <Engine/Graphic/Shader.hpp>
#include <Engine/Graphic/RenderPass.hpp>
#include <Engine/Graphic/Framebuffer.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/VertexBuffer.hpp>

#include <Common/Core/Memory/Buffer.hpp>

// For NO_DISCARD and BOOLSUCCESS on the compute-pipeline rule below. Not implicit: this header is
// reached by translation units that never include Core.hpp on their own — the same reason
// IndexBuffer.hpp and VertexBuffer.hpp include it explicitly.
#include <Common/Core/Core.hpp>

#include <mutex>
#include <unordered_set>

namespace Desert::ShaderResources
{
    class StorageBuffer;
}

namespace Desert::Graphic
{
    class GpuBatch;

    enum class PipelineType
    {
        Graphics,
        Compute
    };

    /**
     * @brief Base interface for all pipeline states.
     */
    class IPipeline;

    namespace detail
    {
        struct LivePipelineSet
        {
            // Recursive: a rebuild pass holds it while each rebuild re-enters through RecordShaderCodeGeneration.
            std::recursive_mutex           Mutex;
            std::unordered_set<IPipeline*> All;
        };

        inline LivePipelineSet& LivePipelines()
        {
            static LivePipelineSet s_Live;
            return s_Live;
        }
    } // namespace detail

    class IPipeline
    {
    public:
        // The ledger row — see Engine/Graphic/ResourceLedger.hpp. The KIND is passed by the two leaves
        // below because a `VkPipeline` from a graphics pass and one from a compute dispatch are different
        // populations to both consumers (eviction never touches either; device-loss recovery rebuilds them
        // by different routes).
        explicit IPipeline( const ResourceKind kind ) : m_Accounting( ResourceOwnership::Take( kind ) )
        {
        }

        // EVERY BUILT PIPELINE IS FINDABLE (RebuildPipelinesBehindTheirShader): a pipeline a renderer built once at
        // init and holds in a member is exactly as much the shader's dependent as one in a PipelineCache. Entered
        // at the first build (the object is whole by then), left here.
        virtual ~IPipeline()
        {
            auto&           live = detail::LivePipelines();
            std::lock_guard lock( live.Mutex );
            live.All.erase( this );
        }

        IPipeline( const IPipeline& )            = delete;
        IPipeline& operator=( const IPipeline& ) = delete;

        virtual void Invalidate() = 0;
        virtual void Release()    = 0;

        [[nodiscard]] virtual PipelineType GetType() const = 0;
        [[nodiscard]] virtual const std::shared_ptr<Shader>& GetShader() const = 0;

        void ClaimOwnership( const ResourceOwner owner, const Common::AssetHandle asset = Common::AssetHandle{} )
        {
            m_Accounting.Claim( owner, asset );
        }

        // Whether the shader's code moved on since this pipeline was last built (Shader::GetCodeGeneration).
        [[nodiscard]] bool IsBehindItsShader() const
        {
            const auto& shader = GetShader();
            return shader && shader->GetCodeGeneration() != m_BuiltAgainstGeneration;
        }

    protected:
        // Each backend calls this at the top of its build, so the recorded version is the one the build read.
        void RecordShaderCodeGeneration()
        {
            const auto& shader       = GetShader();
            m_BuiltAgainstGeneration = shader ? shader->GetCodeGeneration() : 0;
            auto&           live     = detail::LivePipelines();
            std::lock_guard lock( live.Mutex );
            live.All.insert( this );
        }

    private:
        ResourceOwnership m_Accounting;
        uint64_t          m_BuiltAgainstGeneration = 0;
    };

    /**
     * THE ONE ANSWER TO "A SHADER RECOMPILED — WHO HAS TO FOLLOW?" (UE: a recompiled shader map invalidates the
     * PSOs built from it, material and global alike). Rebuilds IN PLACE every live pipeline, graphics or
     * compute, whose shader's code generation moved since it was built — the same object, so a renderer that
     * holds it in a member (DeferredLighting, every compute pass) draws the new code next frame without
     * knowing a reload happened. Returns how many were rebuilt.
     *
     * The caller must have the device idle (a rebuilt pipeline's old VkPipeline may still be in flight) and
     * no pipeline compile running on a worker (PipelineBuilds::WaitIdle).
     */
    size_t RebuildPipelinesBehindTheirShader();

    /// Whether any live pipeline is behind its shader — asked first so a poll that changed nothing does not
    /// idle the device.
    [[nodiscard]] bool AnyPipelineBehindItsShader();

    // --- Graphics Pipeline ---

    enum class StencilOp
    {
        Keep = 0, Zero, Replace, IncrementAndClamp, DecrementAndClamp, Invert, IncrementAndWrap, DecrementAndWrap
    };

    enum class CompareOp
    {
        Never = 0, Less, Equal, LessOrEqual, Greater, NotEqual, GreaterOrEqual, Always
    };

    // Depth tests named by what they MEAN, not by the arithmetic they perform. The engine renders
    // REVERSED-Z (Core/Projection.hpp): the near plane stores 1 and the far plane 0, so "the nearer
    // fragment wins" is CompareOp::Greater, and a pass that writes `DepthCompareOp = CompareOp::Less`
    // renders behind everything and looks like it was never submitted.
    //
    // These are the only two depth compares any pass in the engine has ever wanted, and a pass that
    // spells its intent with one of them stays correct if the convention is ever revisited. Raw
    // CompareOp values remain right for STENCIL, which reversed-Z does not touch.
    namespace DepthCompare
    {
        inline constexpr CompareOp Closer        = CompareOp::Greater;
        inline constexpr CompareOp CloserOrEqual = CompareOp::GreaterOrEqual;
    } // namespace DepthCompare

    enum class CullMode
    {
        None = 0, Front, Back, FrontAndBack
    };

    enum class BlendFactor
    {
        Zero = 0, One, SrcColor, OneMinusSrcColor, DstColor, OneMinusDstColor,
        SrcAlpha, OneMinusSrcAlpha, DstAlpha, OneMinusDstAlpha
    };

    struct StencilOpState
    {
        StencilOp FailOp      = StencilOp::Keep;
        StencilOp PassOp      = StencilOp::Keep;
        StencilOp DepthFailOp = StencilOp::Keep;
        CompareOp CompareOp   = CompareOp::Always;
        uint32_t  CompareMask = 0xFF;
        uint32_t  WriteMask   = 0xFF;
        uint32_t  Reference   = 0;
    };

    enum class PrimitiveTopology
    {
        Points = 0, Lines, Triangles, LineStrip, TriangleStrip, TriangleFan, Patches
    };

    inline bool PrimitiveIsLine( PrimitiveTopology topology )
    {
        return topology == PrimitiveTopology::Lines || topology == PrimitiveTopology::LineStrip;
    }

    enum class PrimitivePolygonMode
    {
        Solid = 0, Wireframe
    };

    struct VertexPullingAttribute
    {
        ShaderDataType Type;
        std::string    Name;
        uint32_t       Offset;
        uint32_t       BindingPoint;
    };

    struct VertexPullingConfig
    {
        uint32_t                            VertexStride = 0;
        std::vector<VertexPullingAttribute> Attributes;
        BufferUsage                         Usage = BufferUsage::Dynamic;
    };

    struct GraphicsPipelineSpecification
    {
        std::shared_ptr<Shader>            Shader;
        std::shared_ptr<Framebuffer>       Framebuffer;
        std::shared_ptr<RenderPass>        Renderpass;
        std::optional<VertexBufferLayout>  Layout;
        std::optional<VertexPullingConfig> PullingConfig;

        bool           DepthTestEnabled   = true;
        CompareOp      DepthCompareOp     = DepthCompare::Closer;
        bool           StencilTestEnabled = false;
        StencilOpState StencilFront;
        StencilOpState StencilBack;
        CullMode       CullMode          = CullMode::None;
        bool           DepthWriteEnabled = true;
        // Standard src-alpha / one-minus-src-alpha blending (transparency overlays, e.g. the scene grid).
        bool           BlendEnable       = false;
        // Color blend factors used when BlendEnable is on. Defaults reproduce the previous hardcoded alpha
        // blend, so shaders that don't declare custom factors render identically. Alpha channel mirrors these.
        BlendFactor    SrcColorBlendFactor = BlendFactor::SrcAlpha;
        BlendFactor    DstColorBlendFactor = BlendFactor::OneMinusSrcAlpha;

        // Build the pipeline against the target framebuffer's LOAD render pass instead of the CLEAR one, so
        // the pass can be begun with clearFrame=false (preserve existing content) without a render-pass
        // incompatibility. Used by the deferred lighting pass to composite over the forward-rendered scene.
        bool           UseLoadRenderPass = false;

        float                LineWidth   = 1.0F;
        PrimitiveTopology    Topology    = PrimitiveTopology::Triangles;
        PrimitivePolygonMode PolygonMode = PrimitivePolygonMode::Solid;

        // > 0 enables tessellation: topology becomes a patch list with this many control points per patch
        // (the pipeline must have tessellation control + evaluation stages).
        uint32_t PatchControlPoints = 0;

        std::string DebugName;
    };

    /**
     * Whether a graphics pipeline may be built from @p spec at all — the same arrangement, and for the
     * same reasons, as CheckComputePipelineSpecification below: one rule, asked in one place, so that
     * GraphicsPipeline::Create has a single thing to obey and a test has a single thing to run.
     *
     * WHY IT IS A FREE FUNCTION IN THE HEADER AND NOT THREE LINES INSIDE Create(). Create() cannot be
     * linked without the whole Vulkan backend, so a gate over it could never fire on a machine with no
     * device — and a refusal nobody can make fire is decoration. Here the DECISION is device-free and
     * Desert/Tests/Engine/GraphicsPipelineRefusal calls it directly.
     *
     * THE THREE DOORS IT CLOSES, each of which was a crash and not a refusal before Г22:
     *   * NO SHADER AT ALL. Four renderers (Tonemap, FXAA, SMAA, Skybox) took the result of
     *     ShaderService::GetByName without looking at it and put it straight into a spec. The backend's
     *     own guard read `Shader && !IsCompiled()`, so a NULL shader sailed past it into
     *     CreatePipelineLayout, which dereferences it — a null-pointer call, not a bad pipeline.
     *   * A SHADER THAT NEVER COMPILED. Its stage list is empty; vkCreateGraphicsPipelines answers
     *     stageCount = 0 with a validation storm. The backend has refused this since its own crash, but
     *     could not tell the caller, so the caller kept the object and drew nothing forever.
     *   * NO FRAMEBUFFER. VulkanPipeline::CreateGraphicsPipeline THREW `std::runtime_error` for this,
     *     which nothing in the engine catches: the refusal was std::terminate.
     *
     * It answers only what can be answered without a device. "Does this program have a VERTEX stage" is
     * a different question, needs the backend's stage list, and is asked by VulkanPipeline::Invalidate
     * through VulkanShader::GetVertexStage — the mirror of the compute side's GetComputeStage.
     */
    NO_DISCARD inline Common::BoolResultStr
    CheckGraphicsPipelineSpecification( const GraphicsPipelineSpecification& spec )
    {
        // A NAMED COPY, not a string_view over the ternary: the arms are `const char[]` and
        // `std::string`, so the ternary itself yields a std::string TEMPORARY, and a view of it would
        // dangle at the semicolon.
        const std::string name = spec.DebugName.empty() ? "<unnamed>" : spec.DebugName;
        if ( !spec.Shader )
        {
            return Common::MakeFormattedError( "GraphicsPipeline '{}': no shader was given.", name );
        }
        if ( !spec.Shader->IsCompiled() )
        {
            return Common::MakeFormattedError(
                 "GraphicsPipeline '{}': shader '{}' has no compiled stages (see the shader compilation "
                 "error above). The draws using it are skipped.",
                 name, spec.Shader->GetName() );
        }
        if ( !spec.Framebuffer )
        {
            return Common::MakeFormattedError(
                 "GraphicsPipeline '{}': no target framebuffer was given; a graphics pipeline is built "
                 "against its target's render pass and cannot exist without one.",
                 name );
        }
        return BOOLSUCCESS;
    }

    // Where a pipeline's driver compile stands, as the draw path needs to know it (AL1-12): only Ready draws
    // through its own pipeline; Compiling and Failed draw the engine's default surface instead.
    enum class PipelineReadiness : uint8_t
    {
        Compiling,
        Ready,
        Failed,
    };

    class GraphicsPipeline : public IPipeline
    {
    public:
        GraphicsPipeline() : IPipeline( ResourceKind::GraphicsPipeline )
        {
        }

        [[nodiscard]] virtual const GraphicsPipelineSpecification& GetSpecification() const = 0;
        [[nodiscard]] virtual PipelineReadiness                    GetReadiness() const     = 0;

        /**
         * The ONLY way to obtain a graphics pipeline, and it hands back one that is already BUILT.
         *
         * IT RETURNS A RESULT BECAUSE IT CAN REFUSE, AND IT COULD NOT BEFORE. It used to return a bare
         * `shared_ptr` that `make_shared` can never leave null, so every one of the thirty-five call
         * sites in the engine was either unchecked or carried an `if ( !pipeline )` branch that COULD
         * NOT RUN. That is what hid the three crashes listed on the rule above: the code looked like it
         * handled a failure it was structurally unable to see.
         *
         * BUILT, NOT MERELY ALLOCATED. Every call site used to read `Create(...)` then `Invalidate()` on
         * the next line — a two-step whose first half returns an object that cannot draw and whose second
         * half could not report anything. Folding it here is what makes the returned pipeline's success a
         * property of the OBJECT rather than of the caller having remembered the second line.
         */
        NO_DISCARD static Common::ResultStr<std::shared_ptr<GraphicsPipeline>>
        Create( const GraphicsPipelineSpecification& spec );

        /**
         * Create's refusals, synchronously, and then the driver compile on the JobSystem (PSO1). The
         * pipeline draws from the frame its compile lands; until then the draws through it are skipped and
         * counted by PipelineBuilds, which ContentGate waits on. A driver refusal on the worker is logged
         * with its VkResult and leaves the pipeline unbuilt (reported by name at the first draw).
         * PipelineCache::GetOrCreate is the one caller: material and mesh pipelines, which appear with
         * content. Pipelines a system builds once at renderer start keep the synchronous Create. @p role
         * decides whether the content gate waits for the compile (PipelineBuilds).
         */
        NO_DISCARD static Common::ResultStr<std::shared_ptr<GraphicsPipeline>>
        CreateAsync( const GraphicsPipelineSpecification& spec, PipelineRole role );
    };

    // --- Compute Pipeline ---

    struct ComputePipelineSpecification
    {
        std::shared_ptr<Shader> Shader;
        std::string             DebugName;
    };

    /**
     * Whether a compute pipeline may be built from @p spec at all — the one rule, asked in one place,
     * so that ComputePipeline::Create has a single thing to obey and a test has a single thing to run.
     *
     * THE REFUSAL THIS EXISTS FOR. A shader whose FIRST compile fails is still constructed, still kept
     * under its name by ShaderService::Register (deliberately, so the material naming it is not silently
     * swapped for the standard one), and still handed out by GetByName(). Building a compute pipeline
     * from it read `VulkanShader::GetPipelineShaderStageCreateInfos()[0]` — of the vector a failed
     * compile leaves EMPTY. Measured on the live editor: 23 validation errors about a descriptor pool
     * with `descriptorSetCount == 0`, then SIGSEGV, i.e. the editor vanished because an artist mistyped
     * a line of GLSL. The graphics side has refused this since its own crash (VulkanPipeline::Invalidate);
     * the compute side had nothing.
     *
     * WHY IT IS A FREE FUNCTION IN THE HEADER AND NOT THREE LINES INSIDE Create(). Create() cannot be
     * linked without the whole Vulkan backend, so a gate over it could never run on a machine with no
     * device — and a refusal nobody can make fire is the kind of gate this project has already been
     * burned by. Here the DECISION is device-free and Desert/Tests/Engine/ComputePipelineRefusal calls
     * it directly; the same suite pins that Create obeys it.
     *
     * It answers only what can be answered without a device: is there a shader, and has it ever
     * compiled. "Is that shader's stage a COMPUTE stage" is a different question, needs the backend's
     * stage list, and is asked by VulkanPipelineCompute::Invalidate through VulkanShader::GetComputeStage.
     */
    NO_DISCARD inline Common::BoolResultStr
    CheckComputePipelineSpecification( const ComputePipelineSpecification& spec )
    {
        if ( !spec.Shader )
        {
            return Common::MakeFormattedError( "ComputePipeline '{}': no shader was given.",
                                               spec.DebugName.empty() ? "<unnamed>" : spec.DebugName );
        }
        if ( !spec.Shader->IsCompiled() )
        {
            return Common::MakeFormattedError(
                 "ComputePipeline '{}': shader '{}' has no compiled stages (see the shader compilation "
                 "error above). The dispatches using it are skipped.",
                 spec.DebugName.empty() ? "<unnamed>" : spec.DebugName, spec.Shader->GetName() );
        }
        return BOOLSUCCESS;
    }

    class ComputePipeline : public IPipeline
    {
    public:
        ComputePipeline() : IPipeline( ResourceKind::ComputePipeline )
        {
        }

        [[nodiscard]] virtual const ComputePipelineSpecification& GetSpecification() const = 0;

        // --- Resource-binding API (UE-style): set inputs/outputs/push-constants, then Dispatch ---

        /** Bind a sampled input image at @p binding (e.g. a panorama or a source cubemap). */
        virtual ComputePipeline& SetInput( uint32_t binding, Image* image ) = 0;
        /** Bind a writable storage output image at @p binding; @p mip selects the target mip view. */
        virtual ComputePipeline& SetOutput( uint32_t binding, Image* image, uint32_t mip = 0 ) = 0;
        /** Bind a read-write storage buffer at @p binding (e.g. a luminance histogram). */
        virtual ComputePipeline& SetStorageBuffer( uint32_t binding, ShaderResources::StorageBuffer* buffer ) = 0;
        /** Set the raw push-constant block used by the next Dispatch (e.g. prefilter roughness). */
        virtual ComputePipeline& SetPushConstants( const void* data, uint32_t size ) = 0;
        /** Record + submit one immediate compute dispatch with the currently-bound resources. */
        virtual void Dispatch( uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ ) = 0;
        /** Record one dispatch into @p batch with the currently-bound resources — no submit, no wait. The
         *  outputs end in SHADER_READ with the writes visible to every later command of the queue. The
         *  pipeline must outlive the batch's completion (GpuBatch::Retain). */
        virtual void Record( GpuBatch& batch, uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ ) = 0;

        // GetInput/GetOutput were here and had no caller: a compute pipeline's bindings are SET and then
        // dispatched, never read back, and the backend keeps its own maps for the descriptor writes. Г12.

        /**
         * The ONLY way to obtain a compute pipeline, and it hands back one that is already BUILT.
         *
         * IT RETURNS A RESULT BECAUSE IT CAN REFUSE, AND IT COULD NOT BEFORE. It used to return a bare
         * `shared_ptr` that `make_shared` can never leave null, so every one of the sixteen call sites
         * in the engine was either unchecked or carried an `if ( !pipeline )` branch that COULD NOT RUN —
         * and that is what hid the crash: the code looked like it handled a failure it was structurally
         * unable to see. Two refusals are now reachable through it: a shader that has never compiled
         * (CheckComputePipelineSpecification above) and one with no COMPUTE stage in it, e.g. a graphics
         * program reached by name (VulkanPipelineCompute::Invalidate, observed here through the built
         * handle rather than through a mirror flag).
         *
         * BUILT, NOT MERELY ALLOCATED. Every call site used to read `Create(...)` then `Invalidate()` on
         * the next line — a two-step whose first half returns an object that cannot dispatch and whose
         * second half could not report anything. Folding it here is what makes the returned pipeline's
         * success a property of the OBJECT rather than of the caller having remembered the second line.
         */
        NO_DISCARD static Common::ResultStr<std::shared_ptr<ComputePipeline>>
        Create( const ComputePipelineSpecification& spec );
    };

} // namespace Desert::Graphic
