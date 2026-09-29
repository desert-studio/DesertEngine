#pragma once

#include <Engine/Graphic/PipelineBuilds.hpp>

#include <chrono>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Core/Formats/ShaderProgramMeta.hpp>

#include <memory>
#include <unordered_map>

namespace Desert::Graphic
{
    // Overlays a shader's `#pragma state` declarations onto a pipeline spec. Only fields the shader
    // actually declared (std::optional set) are applied — everything else keeps the renderer's default.
    // This lets a .shader own its pipeline state (e.g. terrain declaring patch-list topology) instead of
    // every renderer hardcoding it.
    inline void ApplyShaderRenderState( GraphicsPipelineSpecification&          spec,
                                        const Core::Formats::ShaderRenderState& state )
    {
        using namespace Core::Formats;

        if ( state.Cull )
        {
            switch ( *state.Cull )
            {
                case StateCull::None:         spec.CullMode = CullMode::None; break;
                case StateCull::Front:        spec.CullMode = CullMode::Front; break;
                case StateCull::Back:         spec.CullMode = CullMode::Back; break;
                case StateCull::FrontAndBack: spec.CullMode = CullMode::FrontAndBack; break;
            }
        }
        if ( state.DepthTest )
            spec.DepthTestEnabled = *state.DepthTest;
        if ( state.DepthWrite )
            spec.DepthWriteEnabled = *state.DepthWrite;
        if ( state.DepthCompare )
        {
            // `ZTest Less` IS MIRRORED HERE, and this is the whole reason the mapping is not a
            // one-to-one table.
            //
            // A .shader authors its depth test the way every shading language does — `ZTest Less` means
            // "the fragment in front wins", `ZTest LEqual` means "in front, or exactly coplanar". That
            // spelling is what a shader author knows and what every other engine's documentation means
            // by it. The engine, however, renders REVERSED-Z (Core/Projection.hpp), where in front is
            // the LARGER stored value, so the arithmetic test that implements "Less" is Greater.
            //
            // Doing it here rather than by rewriting the five .shader files that spell a compare is not
            // tidiness. The next shader will be written by copying a neighbour, and if the neighbours
            // said `ZTest Greater` the copy would too, and it would be invisible — a whole material
            // rendered behind the world with nothing to point at. The DSL keeps ONE meaning; the
            // convention is applied once, at the boundary, in the only place that knows about pipelines.
            //
            // STENCIL COMPARES ARE NOT MIRRORED. The stencil buffer has no depth convention; its
            // StateCompare goes through `toCompare` below, unchanged, deliberately.
            // clang-format off
            // Kept one-case-per-line to read as the table it is, and to sit beside the identically
            // shaped `toCompare` for stencil below — which is the comparison a reader needs to make.
            const auto mirrored = []( StateCompare c )
            {
                switch ( c )
                {
                    case StateCompare::Never:          return CompareOp::Never;
                    case StateCompare::Less:           return CompareOp::Greater;
                    case StateCompare::Equal:          return CompareOp::Equal;
                    case StateCompare::LessOrEqual:    return CompareOp::GreaterOrEqual;
                    case StateCompare::Greater:        return CompareOp::Less;
                    case StateCompare::NotEqual:       return CompareOp::NotEqual;
                    case StateCompare::GreaterOrEqual: return CompareOp::LessOrEqual;
                    case StateCompare::Always:         return CompareOp::Always;
                }
                return CompareOp::Always;
            };
            // clang-format on
            spec.DepthCompareOp = mirrored( *state.DepthCompare );
        }
        if ( state.Blend )
            spec.BlendEnable = *state.Blend;

        const auto toBlendFactor = []( StateBlendFactor f )
        {
            switch ( f )
            {
                case StateBlendFactor::Zero:             return BlendFactor::Zero;
                case StateBlendFactor::One:              return BlendFactor::One;
                case StateBlendFactor::SrcColor:         return BlendFactor::SrcColor;
                case StateBlendFactor::OneMinusSrcColor: return BlendFactor::OneMinusSrcColor;
                case StateBlendFactor::DstColor:         return BlendFactor::DstColor;
                case StateBlendFactor::OneMinusDstColor: return BlendFactor::OneMinusDstColor;
                case StateBlendFactor::SrcAlpha:         return BlendFactor::SrcAlpha;
                case StateBlendFactor::OneMinusSrcAlpha: return BlendFactor::OneMinusSrcAlpha;
                case StateBlendFactor::DstAlpha:         return BlendFactor::DstAlpha;
                case StateBlendFactor::OneMinusDstAlpha: return BlendFactor::OneMinusDstAlpha;
            }
            return BlendFactor::One;
        };
        if ( state.BlendSrc )
            spec.SrcColorBlendFactor = toBlendFactor( *state.BlendSrc );
        if ( state.BlendDst )
            spec.DstColorBlendFactor = toBlendFactor( *state.BlendDst );

        if ( state.StencilTest )
        {
            spec.StencilTestEnabled = *state.StencilTest;
            const auto toStencilOp  = []( StateStencilOp o )
            {
                switch ( o )
                {
                    case StateStencilOp::Keep:           return StencilOp::Keep;
                    case StateStencilOp::Zero:           return StencilOp::Zero;
                    case StateStencilOp::Replace:        return StencilOp::Replace;
                    case StateStencilOp::IncrementClamp: return StencilOp::IncrementAndClamp;
                    case StateStencilOp::DecrementClamp: return StencilOp::DecrementAndClamp;
                    case StateStencilOp::Invert:         return StencilOp::Invert;
                    case StateStencilOp::IncrementWrap:  return StencilOp::IncrementAndWrap;
                    case StateStencilOp::DecrementWrap:  return StencilOp::DecrementAndWrap;
                }
                return StencilOp::Keep;
            };
            const auto toCompare = []( StateCompare c )
            {
                switch ( c )
                {
                    case StateCompare::Never:          return CompareOp::Never;
                    case StateCompare::Less:           return CompareOp::Less;
                    case StateCompare::Equal:          return CompareOp::Equal;
                    case StateCompare::LessOrEqual:    return CompareOp::LessOrEqual;
                    case StateCompare::Greater:        return CompareOp::Greater;
                    case StateCompare::NotEqual:       return CompareOp::NotEqual;
                    case StateCompare::GreaterOrEqual: return CompareOp::GreaterOrEqual;
                    case StateCompare::Always:         return CompareOp::Always;
                }
                return CompareOp::Always;
            };
            StencilOpState os;
            os.CompareOp   = state.StencilCompare ? toCompare( *state.StencilCompare ) : CompareOp::Always;
            os.FailOp      = state.StencilFail ? toStencilOp( *state.StencilFail ) : StencilOp::Keep;
            os.PassOp      = state.StencilPass ? toStencilOp( *state.StencilPass ) : StencilOp::Replace;
            os.DepthFailOp = state.StencilDepthFail ? toStencilOp( *state.StencilDepthFail ) : StencilOp::Keep;
            os.Reference   = state.StencilRef.value_or( 1u );
            spec.StencilFront = os;
            spec.StencilBack  = os;
        }

        if ( state.Topology )
        {
            switch ( *state.Topology )
            {
                case StateTopology::Triangles: spec.Topology = PrimitiveTopology::Triangles; break;
                case StateTopology::Lines:     spec.Topology = PrimitiveTopology::Lines; break;
                case StateTopology::Points:    spec.Topology = PrimitiveTopology::Points; break;
                case StateTopology::Patches:   spec.Topology = PrimitiveTopology::Patches; break;
            }
        }
        if ( state.PatchControlPoints )
            spec.PatchControlPoints = *state.PatchControlPoints;
    }

    // Caches GraphicsPipelines keyed by (shader + target + render-state) so identical requests share one
    // pipeline instead of every renderer creating its own. Owned by SceneRenderer and filled once, when that
    // renderer builds its systems; loading a different scene into the same renderer does NOT clear it,
    // because nothing in the key is scene-derived (see SceneRenderer::Init). Shader hot-reload drops the
    // affected entries through InvalidateByShader after a WaitDeviceIdle. Pipelines arrive from
    // GraphicsPipeline::Create already built — the two-step `Create(); Invalidate();` that used to live
    // here is gone, because its first half handed out an object that could not draw.
    class PipelineCache
    {
    public:
        /**
         * The cached pipeline for @p spec, BUILT — or the reason there is none.
         *
         * IT USED TO HAND BACK A PIPELINE THAT HAD NOT BEEN BUILT, and that is a second defect on top of
         * the one Create carried. `Create(); if ( pipeline ) Invalidate();` cached whatever came back,
         * built or not: an unbuilt pipeline is a live, non-null `shared_ptr` whose VkPipeline is null, so
         * the ten call sites — seven of which tested `if ( !pipeline )`, a branch that could not run —
         * stored it, drew through it every frame for the life of the renderer, and the only thing that
         * ever said so was VulkanRendererAPI::BindGraphicsPipeline, once, from the far end of the frame.
         *
         * THE REFUSAL IS CACHED TOO, and that is deliberate. MeshRenderer::DrawGenericMeshes asks for a
         * pipeline PER SUBMESH GROUP PER FRAME; if a refusal were not remembered, one mistyped shader
         * would re-enter vkCreateGraphicsPipelines and re-log its reason sixty times a second. The cache
         * entry IS the throttle, and shader hot-reload clears it through InvalidateByShader like any
         * other entry, so a fixed shader recovers on its next compile without a special path.
         *
         * A MALFORMED SPEC IS NOT CACHED. Refusals from the device-free rule are the ones whose key is
         * degenerate — a null Shader or Framebuffer is a null pointer IN THE KEY, so two different
         * callers with two different bugs would collide on one entry and the second would be told the
         * first one's story. Those are call-site defects that must be fixed, not results to remember.
         */
        NO_DISCARD Common::ResultStr<std::shared_ptr<GraphicsPipeline>>
                   GetOrCreate( const GraphicsPipelineSpecification& spec )
        {
            return GetOrCreate( spec, PipelineRole::Engine );
        }

        /**
         * The same, for a CONTENT MATERIAL's own shader (AL1-12, UE's PSO precache): the compile is not waited
         * for by the content gate, and the draw path asks the pipeline's readiness and draws the engine's
         * default surface until it is Ready. Only MeshRenderer's material draws call this.
         */
        NO_DISCARD Common::ResultStr<std::shared_ptr<GraphicsPipeline>>
                   GetOrCreateMaterial( const GraphicsPipelineSpecification& spec )
        {
            return GetOrCreate( spec, PipelineRole::Material );
        }

    private:
        NO_DISCARD Common::ResultStr<std::shared_ptr<GraphicsPipeline>>
                   GetOrCreate( const GraphicsPipelineSpecification& spec, const PipelineRole role )
        {
            if ( const auto buildable = CheckGraphicsPipelineSpecification( spec ); !buildable )
                return Common::MakeError<std::shared_ptr<GraphicsPipeline>>( buildable.GetError() );

            const Key key = MakeKey( spec );

            if ( auto it = m_Cache.find( key ); it != m_Cache.end() )
                return it->second;

            // The driver compile goes to a worker (PSO1): this is the path content takes — a material or a
            // mesh appearing — and the frame must not stall on the driver for it.
            const auto requested = std::chrono::steady_clock::now();
            auto       built     = GraphicsPipeline::CreateAsync( spec, role );
            PipelineBuilds::Get().RecordCallerBlock( std::chrono::steady_clock::now() - requested );
            m_Cache.emplace( key, built );
            return built;
        }

    public:
        void Clear()
        {
            m_Cache.clear();
        }

        // How many distinct GPU pipelines this renderer is holding. Logged by SceneRenderer::Init so a run's
        // log says whether a scene load rebuilt them or kept them — the number Г11 is about, readable
        // without re-measuring anything.
        //
        // THE BUILT ONES, not the entries: a remembered refusal occupies a row and is not a GPU pipeline,
        // and this number is read as "what the device is holding". Counting instead of returning size()
        // is what keeps the sentence above true after Г22 gave the map a second kind of row.
        NO_DISCARD size_t Size() const
        {
            size_t built = 0;
            for ( const auto& [key, entry] : m_Cache )
                built += entry.IsSuccess() ? 1 : 0;
            return built;
        }

        // Drops every pipeline built against @p shader — used by shader hot-reload so the next
        // GetOrCreate() rebuilds with the freshly compiled modules. Caller must WaitDeviceIdle first.
        void InvalidateByShader( const void* shader )
        {
            for ( auto it = m_Cache.begin(); it != m_Cache.end(); )
            {
                if ( it->first.Shader == shader )
                    it = m_Cache.erase( it );
                else
                    ++it;
            }
        }

        // Whether these two specs would be served the SAME cached pipeline. This is the cache's entire
        // correctness condition, stated once so it can be asserted without a device: two specs that differ in
        // anything the backend reads must NOT share, and two that differ only in something it never reads
        // must. Exposed for `Desert/Tests/Engine/PipelineCacheKey`, which pins one field per assertion so the
        // next field added to GraphicsPipelineSpecification cannot fall out of the key quietly — which is how
        // UseLoadRenderPass, the vertex layout, the line width and the back stencil face all went missing.
        NO_DISCARD static bool SharesPipeline( const GraphicsPipelineSpecification& a,
                                               const GraphicsPipelineSpecification& b )
        {
            return MakeKey( a ) == MakeKey( b );
        }

        // Equal keys must hash equally; the test asserts this alongside SharesPipeline so a future key field
        // added to `operator==` but forgotten in KeyHash is caught. (An unequal-but-colliding hash is legal.)
        NO_DISCARD static size_t HashOf( const GraphicsPipelineSpecification& s )
        {
            return KeyHash{}( MakeKey( s ) );
        }

    private:
        // THE KEY MUST NAME EVERY FIELD THE BACKEND READS WHEN IT BUILDS THE PIPELINE. Anything the backend
        // reads but the key omits means two different GPU objects collapse onto one cache entry, and whichever
        // spec asked first silently wins for both. That is not hypothetical: `UseLoadRenderPass` picks
        // GetVKRenderPassLoad() over GetVKRenderPass() (VulkanPipeline.cpp:387), and MeshRenderer draws the
        // same shader with it both false and true depending on whether the frame went through the deferred
        // path, so the two collided exactly.
        //
        // What is deliberately NOT here, and why — this list is the contract, so extend it consciously:
        //   * DebugName: names an object, never changes one. Hashing it would fork a pipeline per label.
        //   * VertexBufferElement::Name and ::Normalized: the backend builds attributes from Type and Offset
        //     only (VulkanPipeline.cpp:242-250), so two layouts differing only in a name ARE the same pipeline.
        //   * VertexPullingConfig's contents: only its ENGAGEMENT reaches the backend — it short-circuits the
        //     vertex input to empty and ignores Layout entirely (VulkanPipeline.cpp:215). Nothing outside
        //     Pipeline.hpp and the Vulkan backend reads the config back, so the contents cannot be observed
        //     through a shared pipeline.
        //   * LineWidth: DYNAMIC. VK_DYNAMIC_STATE_LINE_WIDTH is declared (VulkanPipeline.cpp:284) and
        //     vkCmdSetLineWidth is issued per draw (VulkanRenderer.cpp:443), so Vulkan ignores the baked
        //     value outright and two specs differing only in it really are one pipeline. Hashing it would
        //     fork a pipeline per width for no effect. This one was on the list of "missing" fields until
        //     the backend was read; it is here as a decision, not an oversight.
        //
        // The cache's contract is "specs that produce an equivalent GPU object may share one". A caller that
        // reads its own values back out of pipeline->GetSpecification() is therefore asking the wrong question.
        struct Key
        {
            const void* Shader        = nullptr;
            const void* Framebuffer   = nullptr;
            const void* Renderpass    = nullptr;
            bool        DepthTest     = false;
            bool        DepthWrite    = false;
            bool        StencilTest   = false;
            bool        Blend         = false;
            int         DepthCompare  = 0;
            int         Cull          = 0;
            int         Topology      = 0;
            int         Polygon       = 0;
            uint32_t    PatchPoints   = 0;
            int         BlendSrc      = 0;
            int         BlendDst      = 0;
            // One signature per face over the WHOLE StencilOpState: the four ops, both masks and the
            // reference. The old key packed four ops of the FRONT face only, so a spec that differed in the
            // back face, or in either mask, took another spec's pipeline — and all of compareMask, writeMask
            // and reference are baked into the pipeline (VulkanPipeline.cpp:455-464), not set per draw.
            uint64_t StencilFrontSig = 0;
            uint64_t StencilBackSig  = 0;
            bool     LoadRenderPass  = false;
            bool     VertexPulling   = false; // engaged-ness only; see the note above
            uint32_t LayoutStride    = 0;
            uint32_t LayoutElements  = 0;
            uint64_t LayoutSignature = 0; // FNV-1a over (Type, Offset) of every element, in order

            bool operator==( const Key& ) const = default;
        };

        struct KeyHash
        {
            size_t operator()( const Key& k ) const
            {
                size_t h = 1469598103934665603ull; // FNV offset
                auto   mix = [&h]( size_t v )
                {
                    h ^= v;
                    h *= 1099511628211ull; // FNV prime
                };
                mix( reinterpret_cast<size_t>( k.Shader ) );
                mix( reinterpret_cast<size_t>( k.Framebuffer ) );
                mix( reinterpret_cast<size_t>( k.Renderpass ) );
                mix( ( k.DepthTest ? 1u : 0u ) | ( k.DepthWrite ? 2u : 0u ) | ( k.StencilTest ? 4u : 0u ) |
                     ( k.Blend ? 8u : 0u ) );
                mix( static_cast<size_t>( k.DepthCompare ) );
                mix( static_cast<size_t>( k.Cull ) );
                mix( static_cast<size_t>( k.Topology ) );
                mix( static_cast<size_t>( k.Polygon ) );
                mix( static_cast<size_t>( k.PatchPoints ) );
                mix( static_cast<size_t>( k.BlendSrc ) );
                mix( static_cast<size_t>( k.BlendDst ) );
                mix( static_cast<size_t>( k.StencilFrontSig ) );
                mix( static_cast<size_t>( k.StencilBackSig ) );
                mix( ( k.LoadRenderPass ? 1u : 0u ) | ( k.VertexPulling ? 2u : 0u ) );
                mix( static_cast<size_t>( k.LayoutStride ) );
                mix( static_cast<size_t>( k.LayoutElements ) );
                mix( static_cast<size_t>( k.LayoutSignature ) );
                return h;
            }
        };

        static Key MakeKey( const GraphicsPipelineSpecification& s )
        {
            Key k;
            k.Shader       = s.Shader.get();
            k.Framebuffer  = s.Framebuffer.get();
            k.Renderpass   = s.Renderpass.get();
            k.DepthTest    = s.DepthTestEnabled;
            k.DepthWrite   = s.DepthWriteEnabled;
            k.StencilTest  = s.StencilTestEnabled;
            k.Blend        = s.BlendEnable;
            k.DepthCompare = static_cast<int>( s.DepthCompareOp );
            k.Cull         = static_cast<int>( s.CullMode );
            k.Topology     = static_cast<int>( s.Topology );
            k.Polygon      = static_cast<int>( s.PolygonMode );
            k.PatchPoints  = s.PatchControlPoints;
            k.BlendSrc     = s.BlendEnable ? static_cast<int>( s.SrcColorBlendFactor ) : 0;
            k.BlendDst     = s.BlendEnable ? static_cast<int>( s.DstColorBlendFactor ) : 0;
            if ( s.StencilTestEnabled )
            {
                const auto pack = []( const StencilOpState& f )
                {
                    uint64_t   sig = 1469598103934665603ull;
                    const auto mix = [&sig]( uint64_t v )
                    {
                        sig ^= v;
                        sig *= 1099511628211ull;
                    };
                    mix( static_cast<uint64_t>( f.CompareOp ) );
                    mix( static_cast<uint64_t>( f.FailOp ) );
                    mix( static_cast<uint64_t>( f.PassOp ) );
                    mix( static_cast<uint64_t>( f.DepthFailOp ) );
                    mix( f.CompareMask );
                    mix( f.WriteMask );
                    mix( f.Reference );
                    return sig;
                };
                k.StencilFrontSig = pack( s.StencilFront );
                k.StencilBackSig  = pack( s.StencilBack );
            }

            k.LoadRenderPass = s.UseLoadRenderPass;
            k.VertexPulling  = s.PullingConfig.has_value();

            // Vertex pulling makes the backend ignore Layout outright, so folding it in here would fork
            // pipelines that are in fact identical.
            if ( !k.VertexPulling && s.Layout )
            {
                k.LayoutStride   = s.Layout->GetStride();
                k.LayoutElements = s.Layout->GetElementCount();

                uint64_t sig = 1469598103934665603ull;
                for ( const auto& e : *s.Layout )
                {
                    sig ^= static_cast<uint64_t>( e.Type );
                    sig *= 1099511628211ull;
                    sig ^= static_cast<uint64_t>( e.Offset );
                    sig *= 1099511628211ull;
                }
                // The optional streams binding (VertexBufferLayout::WithStreams) is a different vertex-input
                // state: two layouts that differ only there are two pipelines.
                sig ^= static_cast<uint64_t>( s.Layout->GetStreamStride() );
                sig *= 1099511628211ull;
                sig ^= static_cast<uint64_t>( s.Layout->GetStreamFirstLocation() );
                sig *= 1099511628211ull;
                for ( const auto& e : s.Layout->GetStreamElements() )
                {
                    sig ^= static_cast<uint64_t>( e.Type ) | ( static_cast<uint64_t>( e.Normalized ) << 8 );
                    sig *= 1099511628211ull;
                    sig ^= static_cast<uint64_t>( e.Offset );
                    sig *= 1099511628211ull;
                }
                k.LayoutSignature = sig;
            }
            return k;
        }

        // The OUTCOME per key, not the pipeline: see GetOrCreate for why a refusal is a row here too.
        std::unordered_map<Key, Common::ResultStr<std::shared_ptr<GraphicsPipeline>>, KeyHash> m_Cache;
    };

} // namespace Desert::Graphic
