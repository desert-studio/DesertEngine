// MeshRenderer's surface-cache card capture (GI-SURFACE-CACHE-2): the frame's card instances and the G-buffer
// light-view draws that capture the cards SurfaceCacheScene picked (UE LumenSceneCardCapture.cpp).
#include "MeshRendererInternal.hpp"

#include <Engine/Graphic/Renderer.hpp>

#include <Engine/Geometry/MeshBounds.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <format>
#include <unordered_map>

namespace Desert::Graphic::System
{
    namespace
    {
        uint64_t HashCombine( uint64_t seed, const uint64_t value )
        {
            return ( seed ^ value ) * 0x100000001B3ull;
        }
        uint64_t PointerBits( const void* p )
        {
            return static_cast<uint64_t>( reinterpret_cast<std::uintptr_t>( p ) );
        }
    } // namespace

    // The (path x GBuffer) pass state a card of that path is captured with; null when the renderer has none.
    GraphicsPipeline* MeshRenderer::CardCapturePassState( const MeshVertexPath path ) const
    {
        switch ( path )
        {
            case MeshVertexPath::Static:
                return m_StaticGBufferPipeline.get();
            case MeshVertexPath::Instanced:
                return m_InstancedGBufferPipeline.get();
            case MeshVertexPath::Skinned:
                return m_SkinnedGBufferPipeline.get();
            default:
                return nullptr;
        }
    }

    void MeshRenderer::GatherSurfaceCacheInstances( std::vector<SurfaceCache::SceneInstance>& out )
    {
        m_CardCaptureSources.clear();
        auto* const materials = Runtime::ResourceRegistry::GetMaterialService();
        auto* const shaders   = Runtime::ResourceRegistry::GetShaderService();
        // A key seen twice this frame (two parts submitted under one entity and part) gets a salt per repeat, so
        // neither replaces the other.
        std::unordered_map<uint64_t, uint32_t> repeats;
        // The surface's identity and whether its G-buffer cell draws this frame. A `.demat` surface is captured by
        // its own (path x GBuffer) cell - the scene G-buffer's program, textures and parameters - and only once
        // that cell's pipeline is Ready (the same readiness the scene's G-buffer draw asks, ChooseCellDraw); the
        // renderer-owned default surface by the pass's own cell, an engine pipeline.
        const auto surfaceOf = [&]( DataDrivenMaterial* surface, MaterialInstance* instance,
                                    const MeshVertexPath path, uint64_t& key, bool& ready )
        {
            key = HashCombine( HashCombine( 0xCBF29CE484222325ull, PointerBits( surface ) ),
                               PointerBits( instance ) );
            GraphicsPipeline* const pass = CardCapturePassState( path );
            ready                        = pass != nullptr;
            if ( materials == nullptr || !materials->Owns( surface ) )
                return;
            DataDrivenMaterial* const cell = materials->GetVariant( surface, path, MeshPass::GBuffer );
            key                            = HashCombine( key, PointerBits( cell ) );
            const bool twoSided            = instance != nullptr ? instance->IsTwoSided() : surface->IsTwoSided();
            ready                          = cell != nullptr && pass != nullptr &&
                    ChooseCellDraw( pass, *cell, twoSided, nullptr ) == MaterialPipelineTracker::CellDraw::Own;
        };
        const auto add = [&]( uint64_t key, const Mesh* mesh,
                              std::shared_ptr<const Assets::MeshCardsBuildData> cards, const glm::mat4& world,
                              CardCaptureSource source, const std::optional<std::pair<uint64_t, bool>>& surface )
        {
            const uint32_t repeat = repeats[key]++;
            if ( repeat > 0 )
                key = ( key * 0x100000001B3ull ) ^ ( 0x4000000000000000ull | repeat );
            SurfaceCache::SceneInstance instance{ .Key    = key,
                                                  .Mesh   = mesh,
                                                  .Cards  = std::move( cards ),
                                                  .World  = world,
                                                  .Source = static_cast<uint32_t>( m_CardCaptureSources.size() ) };
            if ( surface )
            {
                instance.SurfaceKey   = surface->first;
                instance.CaptureReady = surface->second;
            }
            else
                surfaceOf( source.Surface, source.Instance, source.Path, instance.SurfaceKey,
                           instance.CaptureReady );
            out.push_back( std::move( instance ) );
            m_CardCaptureSources.push_back( std::move( source ) );
        };
        // A static mesh is queued once per SECTION (MESH-SECTION-MATS: each section group draws with its own slot,
        // the other sections hidden), but its cards are the mesh's, captured once per entity: the capture keeps
        // b18's rule - the whole mesh through its first surface slot - and hides only what the ENTITY hides, the
        // submeshes hidden in every one of its section entries (each other submesh is visible in exactly one).
        struct StaticCapture
        {
            const StaticMeshRenderData* Data   = nullptr;
            uint64_t                    Hidden = ~0ull;
        };
        std::vector<uint64_t>                       staticOrder;
        std::unordered_map<uint64_t, StaticCapture> staticCaptures;
        for ( const auto& data : m_StaticQueue )
        {
            if ( data.Mesh == nullptr || !data.MaterialSlots || data.MaterialSlots->Slots.empty() )
                continue;
            const uint64_t key = ( static_cast<uint64_t>( data.Entity ) << 24u ) | ( data.MotionPart & 0xFFFFFFu );
            auto [it, inserted] = staticCaptures.try_emplace( key, StaticCapture{ &data, ~0ull } );
            if ( inserted )
                staticOrder.push_back( key );
            it->second.Hidden &= data.HiddenSubmeshes;
        }
        for ( const uint64_t key : staticOrder )
        {
            const StaticCapture& capture = staticCaptures.at( key );
            const auto&          data    = *capture.Data;
            auto                 cards   = data.Mesh->GetCards();
            if ( !cards || cards->Cards.empty() )
                continue;
            // Opaque surfaces only: glass transmits, its cards would hold what is behind it.
            SurfaceSlot slot;
            for ( uint32_t s = 0; s < static_cast<uint32_t>( data.MaterialSlots->Slots.size() ) && !slot; ++s )
                slot = SectionSurface( data.MaterialSlots.get(), s );
            if ( !slot || IsTranslucent( slot.Surface ) )
                continue;
            add( key, data.Mesh, std::move( cards ), data.Transform,
                 { .Mesh      = data.Mesh,
                   .World     = data.Transform,
                   .Instance  = slot.Instance,
                   .Surface   = slot.Surface,
                   .Hidden    = capture.Hidden,
                   .MotionRow = data.MotionRow },
                 std::nullopt );
        }
        // Instanced static meshes and foliage: every instance is an instance of the Lumen scene (UE adds each ISM
        // instance's cards), keyed by its mesh, material and index - stable while the set is; a moved instance
        // keeps its texels.
        for ( const auto& ism : m_InstancedQueue )
        {
            if ( ism.Mesh == nullptr || !ism.Material || !ism.Transforms || ism.Transforms->empty() )
                continue;
            auto* const surface = dynamic_cast<DataDrivenMaterial*>( ism.Material->GetParentMaterial() );
            if ( surface == nullptr || IsTranslucent( surface ) )
                continue;
            const auto cards = ism.Mesh->GetCards();
            if ( !cards || cards->Cards.empty() )
                continue;
            const uint64_t batch = HashCombine( HashCombine( 0x84222325CBF29CE4ull, PointerBits( ism.Mesh ) ),
                                                PointerBits( ism.Material.get() ) );
            for ( std::size_t i = 0; i < ism.Transforms->size(); ++i )
            {
                const glm::mat4& world = ( *ism.Transforms )[i];
                // Bit 63 set: never an entity key (entity << 24 stays below it).
                const uint64_t key = 0x8000000000000000ull | ( HashCombine( batch, i ) >> 1u );
                add( key, ism.Mesh, cards, world,
                     { .Mesh     = ism.Mesh,
                       .World    = world,
                       .Instance = ism.Material.get(),
                       .Surface  = surface,
                       .Path     = MeshVertexPath::Instanced },
                     std::nullopt );
            }
        }
        // GENERIC DRAWS (UE: every primitive is in the Lumen scene): a slot material that is not the standard
        // surface (its `.demat` on another template) and a Shader Override (MaterialComponent: a template named
        // with per-entity values, no `.demat`). Both are captured by their template's (Static x GBuffer) cell -
        // the G-buffer permutation of the program the scene draws them forward with. A program with no Surface
        // block has no such cell (the text system's glyph quads, an unlit program): no cards, as UE keeps a
        // material without a surface out of the surface cache.
        for ( const auto& g : m_GenericQueue )
        {
            if ( g.Mesh == nullptr )
                continue;
            auto cards = g.Mesh->GetCards();
            if ( !cards || cards->Cards.empty() )
                continue;
            const uint64_t    key = ( static_cast<uint64_t>( g.Entity ) << 24u ) | ( g.MotionPart & 0xFFFFFFu );
            CardCaptureSource source{
                 .Mesh = g.Mesh, .World = g.Transform, .Hidden = ~g.VisibleSubmeshMask, .MotionRow = g.MotionRow };
            if ( g.SlotMaterial != nullptr )
            {
                auto* const surface = dynamic_cast<DataDrivenMaterial*>( g.SlotMaterial );
                if ( surface == nullptr || IsTranslucent( surface ) || materials == nullptr ||
                     !materials->Owns( surface ) )
                    continue;
                source.Surface = surface;
                add( key, g.Mesh, std::move( cards ), g.Transform, std::move( source ), std::nullopt );
                continue;
            }
            if ( shaders == nullptr || m_StaticGBufferPipeline == nullptr )
                continue;
            const auto program = shaders->GetByName( g.ShaderName );
            if ( !program || !program->IsCompiled() ||
                 !Core::Formats::DrawnByMeshPath( program->GetProgramMeta().Domain ) )
                continue;
            const auto cell = TemplateCellShader( g.ShaderName, MeshVertexPath::Static, MeshPass::GBuffer,
                                                  [shaders]( std::string_view name ) {
                                                      return shaders->GetByName( std::string( name ) ) != nullptr;
                                                  } );
            if ( !cell || *cell == g.ShaderName )
                continue;
            // The first material of its pool answers the cell's blend mode and readiness.
            auto& pool = m_CardCaptureOverrides[*cell + GenericTextureKey( g )];
            if ( pool.empty() )
                pool.push_back( std::make_shared<DataDrivenMaterial>( *cell ) );
            DataDrivenMaterial& probe = *pool.front();
            if ( IsTranslucent( &probe ) )
                continue;
            const bool ready = ChooseCellDraw( m_StaticGBufferPipeline.get(), probe, probe.IsTwoSided(),
                                               nullptr ) == MaterialPipelineTracker::CellDraw::Own;
            // Its identity: the cell, its textures and its values - an edited override recaptures its cards.
            uint64_t surfaceKey = HashCombine( 0x9E3779B97F4A7C15ull, std::hash<std::string>{}( *cell ) );
            surfaceKey          = HashCombine( surfaceKey, std::hash<std::string>{}( GenericTextureKey( g ) ) );
            for ( const auto& [name, value] : g.Overrides.Params )
            {
                surfaceKey = HashCombine( surfaceKey, std::hash<std::string>{}( name ) );
                for ( int lane = 0; lane < 4; ++lane )
                    surfaceKey = HashCombine( surfaceKey, std::hash<float>{}( value[lane] ) );
            }
            source.Generic      = &g;
            source.OverrideCell = *cell;
            add( key, g.Mesh, std::move( cards ), g.Transform, std::move( source ),
                 std::pair{ surfaceKey, ready } );
        }
        // SKINNED MESHES (UE MeshCardRepresentation SetCardsFromBounds): a skinned mesh has no card build; its
        // cards are the six faces of its bind-pose box, captured through its (Skinned x GBuffer) cell in the pose
        // of the frame that captures them. Bit 62 set: never an entity key of the static or generic queue.
        for ( const auto& data : m_SkinnedQueue )
        {
            // The capture draws the whole pose through the first opaque section's material (as the static rule).
            const SkinnedSection* section = nullptr;
            for ( const auto& candidate : data.Sections )
                if ( candidate.Material != nullptr && candidate.Instance != nullptr &&
                     !IsTranslucent( candidate.Material ) )
                {
                    section = &candidate;
                    break;
                }
            if ( data.Mesh == nullptr || section == nullptr )
                continue;
            auto& cards = m_SkinnedBoundsCards[data.Mesh];
            if ( !cards )
            {
                const Common::Math::AABB box = Geometry::LocalBounds( data.Mesh->GetSubmeshes() );
                if ( Geometry::IsEmpty( box ) )
                    continue;
                auto built       = std::make_shared<Assets::MeshCardsBuildData>();
                built->BoundsMin = box.Min;
                built->BoundsMax = box.Max;
                for ( uint32_t direction = 0; direction < Assets::kCardDirections; ++direction )
                    built->Cards.push_back( { .Center    = 0.5f * ( box.Min + box.Max ),
                                              .Extent    = 0.5f * ( box.Max - box.Min ),
                                              .Direction = static_cast<Assets::CardDirection>( direction ) } );
                cards = std::move( built );
            }
            const uint64_t key = 0x4000000000000000ull | ( static_cast<uint64_t>( data.Entity ) << 24u );
            add( key, data.Mesh, cards, data.Transform,
                 { .Mesh      = data.Mesh,
                   .World     = data.Transform,
                   .Instance  = section->Instance,
                   .Surface   = section->Material,
                   .MotionRow = data.MotionRow,
                   .Path      = MeshVertexPath::Skinned },
                 std::nullopt );
        }
    }

    void MeshRenderer::DeclareCardCaptureDraws( RDG::PassBuilder&                             pass,
                                                const std::vector<SurfaceCache::CardCapture>& captures )
    {
        m_CardCaptureDraws.Clear();
        if ( captures.empty() )
            return;
        auto* const materials = Runtime::ResourceRegistry::GetMaterialService();
        // The k-th capture of a surface in this frame draws through that surface's k-th view copy (its camera
        // block holds this card's camera); the renderer-owned surface through the k-th material of its cell's
        // pool; a Shader Override through the k-th material of its cell's pool, its overrides written in.
        std::unordered_map<const DataDrivenMaterial*, uint32_t> viewsTaken;
        std::unordered_map<const void*, uint32_t>               overridesTaken;
        uint32_t                                                pooled[3] = { 0u, 0u, 0u };
        for ( const SurfaceCache::CardCapture& capture : captures )
        {
            const CardCaptureSource& source    = m_CardCaptureSources[capture.Source];
            const MeshVertexPath     path      = source.Path;
            const uint32_t           cell      = static_cast<uint32_t>( path );
            GraphicsPipeline* const  passState = CardCapturePassState( path );
            if ( passState == nullptr || cell >= 3u )
            {
                m_CardCaptureDraws.Fail( std::format( "the ({} x GBuffer) pipeline the card capture draws with is "
                                                      "missing",
                                                      MeshVertexPathName( path ) ) );
                return;
            }
            DataDrivenMaterial*    material = nullptr;
            MaterialInstance*      instance = source.Instance;
            std::vector<glm::vec4> rows;
            if ( source.Generic != nullptr )
            {
                auto& pool = m_CardCaptureOverrides[source.OverrideCell + GenericTextureKey( *source.Generic )];
                const uint32_t k = overridesTaken[&pool]++;
                while ( pool.size() <= k )
                    pool.push_back( std::make_shared<DataDrivenMaterial>( source.OverrideCell ) );
                material = pool[k].get();
                ApplyGenericOverrides( *material, *source.Generic, source.OverrideCell );
                const auto& row = material->GetParamRow();
                rows.assign( row.begin(), row.end() );
            }
            else if ( materials != nullptr && materials->Owns( source.Surface ) )
            {
                const uint32_t view = viewsTaken[source.Surface]++;
                material            = materials->GetViewVariant( source.Surface, path, MeshPass::GBuffer, view );
                if ( material == nullptr )
                {
                    // Gathered as ready, so the cell exists: MaterialService named what refused the copy.
                    m_CardCaptureDraws.Fail(
                         std::format( "the ({} x GBuffer) view copy {} of a surface for the card "
                                      "capture was refused",
                                      MeshVertexPathName( path ), view ) );
                    return;
                }
                // Its one row: the surface's effective parameters, as the scene's G-buffer draw packs them.
                AppendRow( rows, EffectiveRow( source.Surface, source.Instance ) );
            }
            else
            {
                auto& pool      = m_CardCaptureMaterials[cell];
                auto& instances = m_CardCaptureInstances[cell];
                if ( pooled[cell] == pool.size() )
                {
                    auto created = CreateCellMaterial( path, MeshPass::GBuffer );
                    if ( !created )
                    {
                        m_CardCaptureDraws.Fail( std::format(
                             "the ({} x GBuffer) cell material for the card capture could not be created",
                             MeshVertexPathName( path ) ) );
                        return;
                    }
                    instances.push_back( created->CreateInstance() );
                    pool.push_back( std::move( created ) );
                }
                material = pool[pooled[cell]].get();
                // The default surface's own instance stands for itself: its overrides are the slot's.
                instance = source.Instance != nullptr ? source.Instance : instances[pooled[cell]].get();
                ++pooled[cell];
                AppendRow( rows, EffectiveRow( source.Surface, source.Instance ) );
            }

            if ( !rows.empty() )
                if ( auto* sb = material->Get<StorageBufferProperty>( "Materials" ) )
                    sb->SetRawData( rows.data(), static_cast<uint32_t>( rows.size() * sizeof( glm::vec4 ) ) );
            // An instance's one transform (the instanced cell reads its world from InstanceTransforms).
            if ( path == MeshVertexPath::Instanced )
                if ( auto* sb = material->Get<StorageBufferProperty>( "InstanceTransforms" ) )
                    sb->SetRawData( &source.World, static_cast<uint32_t>( sizeof( glm::mat4 ) ) );
            // The card is not a view of its own (no jitter, no previous frame): its camera, in its material. The
            // frame state also carries the view's ObjectMotions and ObjectBones, which place a static or generic
            // mesh and pose a skinned one by its motion row.
            const ViewFrame cardView =
                 MakeStillViewFrame( capture.View, capture.Projection, capture.Eye, m_WorldTimeSeconds );
            CaptureFrameState( &cardView ).ApplyTo( static_cast<Material*>( material ) );

            const bool twoSided = instance != nullptr ? instance->IsTwoSided() : material->IsTwoSided();
            GraphicsPipeline* const pipeline   = CullPermutation( CellPipeline( passState, *material ), twoSided );
            const SurfaceCache::AtlasRect rect = capture.CaptureRect;
            // The view-pass cells place the mesh themselves (static, generic, skinned: the ObjectMotions row;
            // instanced: the instance transform): the push transform is the submesh's alone, as in the scene's
            // G-buffer draw.
            m_CardCaptureDraws.Add( { .Pipeline          = pipeline,
                                      .Mesh              = source.Mesh,
                                      .Transform         = glm::mat4( 1.0f ),
                                      .Material          = material->GetMaterialExecutor(),
                                      .InstanceCount     = 1u,
                                      .FirstInstance     = 0u,
                                      .HiddenSubmeshMask = source.Hidden,
                                      .BindState         = [material, instance, row = source.MotionRow, path, rect]
                                      {
                                          // Its own rectangle of the capture atlas only (the pass cleared the
                                          // whole atlas: every capture of the frame is new).
                                          Renderer::GetInstance().SetViewportRegion(
                                               static_cast<int32_t>( rect.X ), static_cast<int32_t>( rect.Y ),
                                               rect.Width, rect.Height );
                                          material->SetPushMatrix( glm::mat4( 1.0f ) );
                                          material->SetMaterialIndex( 0u );
                                          if ( path == MeshVertexPath::Instanced )
                                              material->SetInstancedWind( InstanceWindPush{} ); // a still card
                                          else
                                              material->SetPrimitiveIndex( row );
                                          // A Shader Override draw has no instance: ApplyGenericOverrides wrote
                                          // its values into the pooled material, and the scene's generic pass
                                          // draws it without a Bind (Material::Bind reads the instance).
                                          if ( instance != nullptr )
                                              material->Bind( instance );
                                      } } );
        }
        m_CardCaptureDraws.Declare( pass, std::nullopt );
    }

    Common::BoolResultStr MeshRenderer::RenderCardCaptureManual( const RDG::PassContext& context ) const
    {
        return m_CardCaptureDraws.Record( context );
    }
} // namespace Desert::Graphic::System
