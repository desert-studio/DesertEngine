// MeshRenderer's surface-cache card capture (GI-SURFACE-CACHE-2): the frame's card instances and the G-buffer
// light-view draws that capture the cards SurfaceCacheScene picked (UE LumenSceneCardCapture.cpp).
#include "MeshRendererInternal.hpp"

#include <Engine/Graphic/Renderer.hpp>

#include <Engine/Geometry/MeshBounds.hpp>
#include <Engine/Assets/MeshCardRepresentation.hpp>
#include <Engine/Graphic/Pipeline.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <format>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct MeshCardCaptureShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, InstanceTransforms );
    };
} // namespace

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

    uint64_t MeshRenderer::DrawReadinessKey() const
    {
        auto* const materials = Runtime::ResourceRegistry::GetMaterialService();
        auto* const shaders   = Runtime::ResourceRegistry::GetShaderService();
        uint64_t    key       = 0xCBF29CE484222325ull;
        if ( materials != nullptr )
            key = HashCombine( key, static_cast<uint64_t>( materials->GetInvalidationVersion() ) );
        key = HashCombine( key, Graphic::PipelineReadinessGeneration() );
        if ( shaders != nullptr )
            key = HashCombine( key, static_cast<uint64_t>( shaders->RecompileGeneration() ) );
        if ( m_SceneRenderer != nullptr )
            key = HashCombine( key, m_SceneRenderer->GetPipelineCache().Generation() );
        return key;
    }

    uint64_t MeshRenderer::CardReadinessKey() const
    {
        uint64_t key = DrawReadinessKey();
        key          = HashCombine( key, Assets::MeshCardsSlot::PublishGeneration() );
        key = HashCombine( key, PointerBits( m_StaticGBufferPipeline.get() ) );
        key = HashCombine( key, PointerBits( m_InstancedGBufferPipeline.get() ) );
        key = HashCombine( key, PointerBits( m_SkinnedGBufferPipeline.get() ) );
        return key;
    }

    void MeshRenderer::GatherSurfaceCacheInstances( std::vector<SurfaceCache::SceneGroup>& out )
    {
        DESERT_PROFILE_SCOPE( "MeshRenderer::GatherSurfaceCacheInstances" );
        out.clear();
        ++m_CardGather;
        auto* const materials = Runtime::ResourceRegistry::GetMaterialService();
        auto* const shaders   = Runtime::ResourceRegistry::GetShaderService();
        // Read BEFORE any answer below is asked: an event after this read moves the key the next gather reads.
        const uint64_t readiness = CardReadinessKey();
        // The group being gathered, and its instances by their (salted) key as indices into its Instances: a
        // section of a mesh instance already gathered joins it.
        CardGatherGroup*                          target = nullptr;
        std::unordered_map<uint64_t, std::size_t> placed;
        const auto                                startGroup = [&]( CardGatherGroup& group, const uint64_t key )
        {
            target = &group;
            placed.clear();
            group.Instances.clear();
            group.Sources.clear();
            group.StandIns = 0;
            group.Key      = key;
            group.Revision = ++m_CardGroupRevision;
            group.Gathered = true;
        };
        // Whether the cell draws its own pipeline; any other answer is a stand-in the group counts (its kept
        // frames count it again: GetStandInDraws).
        const auto ownCell = [&]( GraphicsPipeline* pass, DataDrivenMaterial& cell, const bool twoSided )
        {
            const bool own =
                 ChooseCellDraw( pass, cell, twoSided, nullptr ) == MaterialPipelineTracker::CellDraw::Own;
            if ( !own )
                ++target->StandIns;
            return own;
        };
        // A section's identity and whether its G-buffer cell draws this frame. A `.demat` surface is captured by
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
            // The asset's build revision, not the cell's address: a material rebuilt bound (its texture landed)
            // may reuse its predecessor's address, and the card captured in the slot defaults would never
            // recapture (GRASS-SETTLE). Not ready while a texture is in flight — as UE's card capture waits for
            // the material's textures to stream — the same gate the scene's own draw asks (AwaitsTextures).
            DataDrivenMaterial* const cell = materials->GetVariant( surface, path, MeshPass::GBuffer );
            key                 = HashCombine( key, static_cast<uint64_t>( materials->BuildRevision( surface ) ) );
            const bool twoSided = instance != nullptr ? instance->IsTwoSided() : surface->IsTwoSided();
            ready               = cell != nullptr && pass != nullptr && !materials->AwaitsTextures( surface ) &&
                    ownCell( pass, *cell, twoSided );
        };
        // ONE CARD SET PER MESH INSTANCE (UE FLumenCardRenderer): a section joins the instance of its key when
        // that instance is the same mesh in the same place on the same path; a different mesh under the same
        // key (two parts submitted under one entity and part) takes the next salt, so neither replaces the
        // other. The instance's surface identity covers every section (a swap of any section's material
        // recaptures its cards), and it is ready to capture only when every section can draw.
        const auto addSection = [&]( const uint64_t key, const Mesh* mesh,
                                     const std::shared_ptr<const Assets::MeshCardsBuildData>& cards,
                                     const glm::mat4& world, const MeshVertexPath path, CardCaptureSection section,
                                     const std::optional<std::pair<uint64_t, bool>>& surface )
        {
            uint64_t sectionKey   = 0;
            bool     sectionReady = false;
            if ( surface )
            {
                sectionKey   = surface->first;
                sectionReady = surface->second;
            }
            else
                surfaceOf( section.Surface, section.Instance, path, sectionKey, sectionReady );
            std::vector<SurfaceCache::SceneInstance>& instances = target->Instances;
            std::vector<CardCaptureSource>&           sources   = target->Sources;
            for ( uint32_t repeat = 0;; ++repeat )
            {
                const uint64_t salted =
                     repeat == 0 ? key : ( ( key * 0x100000001B3ull ) ^ ( 0x4000000000000000ull | repeat ) );
                const auto found = placed.find( salted );
                if ( found == placed.end() )
                {
                    placed.emplace( salted, instances.size() );
                    instances.push_back( { .Key          = salted,
                                           .Mesh         = mesh,
                                           .Cards        = cards,
                                           .World        = world,
                                           .Source       = static_cast<uint32_t>( sources.size() ),
                                           .SurfaceKey   = sectionKey,
                                           .CaptureReady = sectionReady } );
                    CardCaptureSource source{ .Mesh = mesh, .World = world, .Path = path };
                    source.Sections.push_back( std::move( section ) );
                    sources.push_back( std::move( source ) );
                    return;
                }
                SurfaceCache::SceneInstance& instance = instances[found->second];
                CardCaptureSource&           source   = sources[instance.Source];
                if ( instance.Mesh != mesh || source.Path != path || source.World != world )
                    continue;
                instance.SurfaceKey   = HashCombine( instance.SurfaceKey, sectionKey );
                instance.CaptureReady = instance.CaptureReady && sectionReady;
                source.Sections.push_back( std::move( section ) );
                return;
            }
        };
        m_CardGroupOrder.clear();

        // THE STATIC + GENERIC GROUP, gathered again only when the static casters' revision (or the readiness)
        // moved since - or every frame while the revision is not tracked for this gather, or after a gather
        // that met an entry the GpuScene stream does not follow (no primitive row: its changes reach no revision).
        {
            CardGatherGroup& group = m_CardStaticGroup;
            group.Id               = 1u;
            const bool     tracked = m_StaticCasters.Revision != 0u && m_StaticCasters.Gather == m_ShadowCasterSetup.Gather;
            const uint64_t key     = HashCombine( HashCombine( readiness, m_StaticCasters.Revision ), m_StaticCasters.Stream );
            if ( tracked && group.Gathered && group.Key == key )
                m_StandInDraws += group.StandIns;
            else
            {
                startGroup( group, key );
                bool followed = tracked;
                // STATIC MESHES: an entry is one section (its SectionSlot's material by ShadingOf - the answer every
                // other pass draws it with; HiddenSubmeshes masks the other sections'); the entries of one entity
                // and part are one instance.
                // A synced entry's mesh and section surface come from the walk columns (ShadingOf's answer, made
                // by this view's sync): a surfaceless or translucent section costs its column bytes, not a
                // dynamic_cast and the template's path. An entry past them (queued after the sync) asks ShadingOf.
                const StaticWalkColumns& columns = m_StaticColumns;
                for ( std::size_t at = 0; at < m_StaticQueue.size(); ++at )
                {
                    const StaticMeshRenderData& data = m_StaticQueue[at];
                    SectionShading              shading;
                    if ( StaticColumnsHave( at ) )
                    {
                        if ( columns.Meshes[at] == nullptr || columns.Surfaces[at] == nullptr ||
                             ( columns.Flags[at] & StaticWalkColumns::kTranslucent ) != 0u )
                            continue;
                        shading.Instance = columns.Instances[at];
                        shading.Surface  = columns.Surfaces[at];
                    }
                    else
                    {
                        if ( data.Mesh == nullptr )
                            continue;
                        shading = ShadingOf( data );
                        if ( shading.Surface == nullptr || IsTranslucent( shading.Surface ) )
                            continue;
                    }
                    const auto cards = data.Mesh->GetCards();
                    if ( !cards || cards->Cards.empty() )
                        continue;
                    if ( data.GpuPrimitive == GpuDriven::kInvalidIndex )
                        followed = false;
                    const uint64_t entryKey =
                         ( static_cast<uint64_t>( data.Entity ) << 24u ) | ( data.MotionPart & 0xFFFFFFu );
                    addSection( entryKey, data.Mesh, cards, data.Transform, MeshVertexPath::Static,
                                { .Instance     = shading.Instance,
                                  .Surface      = shading.Surface,
                                  .Hidden       = data.HiddenSubmeshes,
                                  .GpuPrimitive = data.GpuPrimitive },
                                std::nullopt );
                }
                // GENERIC DRAWS (UE: every primitive is in the Lumen scene): a slot material that is not the
                // standard surface (its `.demat` on another template) and a Shader Override (MaterialComponent: a
                // template named with per-entity values, no `.demat`). Both are captured by their template's
                // (Static x GBuffer) cell - the G-buffer permutation of the program the scene draws them forward
                // with. A program with no Surface block has no such cell (the text system's glyph quads, an unlit
                // program): no cards, as UE keeps a material without a surface out of the surface cache. Keyed as
                // a static entry: a mesh whose sections are drawn partly by the static queue and partly here is
                // still one instance.
                for ( const auto& g : m_GenericQueue )
                {
                    if ( g.Mesh == nullptr )
                        continue;
                    const auto cards = g.Mesh->GetCards();
                    if ( !cards || cards->Cards.empty() )
                        continue;
                    if ( g.GpuPrimitive == GpuDriven::kInvalidIndex )
                        followed = false;
                    const uint64_t entryKey =
                         ( static_cast<uint64_t>( g.Entity ) << 24u ) | ( g.MotionPart & 0xFFFFFFu );
                    CardCaptureSection section{ .Hidden = ~g.VisibleSubmeshMask, .GpuPrimitive = g.GpuPrimitive };
                    if ( g.SlotMaterial != nullptr )
                    {
                        auto* const surface = dynamic_cast<DataDrivenMaterial*>( g.SlotMaterial );
                        if ( surface == nullptr || IsTranslucent( surface ) || materials == nullptr ||
                             !materials->Owns( surface ) )
                            continue;
                        section.Surface = surface;
                        addSection( entryKey, g.Mesh, cards, g.Transform, MeshVertexPath::Static, std::move( section ),
                                    std::nullopt );
                        continue;
                    }
                    if ( shaders == nullptr || m_StaticGBufferPipeline == nullptr )
                        continue;
                    const auto program = shaders->GetByName( g.ShaderName() );
                    // A program not registered yet: no recompile generation announces its arrival.
                    if ( !program )
                    {
                        followed = false;
                        continue;
                    }
                    if ( !program->IsCompiled() || !Core::Formats::DrawnByMeshPath( program->GetProgramMeta().Domain ) )
                        continue;
                    const std::string& shaderName = g.ShaderName();
                    const auto cell = TemplateCellShader( shaderName, MeshVertexPath::Static, MeshPass::GBuffer,
                                                          [shaders]( std::string_view name ) {
                                                              return shaders->GetByName( std::string( name ) ) != nullptr;
                                                          } );
                    if ( !cell || *cell == g.ShaderName() )
                        continue;
                    // The first material of its pool answers the cell's blend mode and readiness.
                    auto& pool = m_CardCaptureOverrides[*cell + GenericTextureKey( g )];
                    if ( pool.empty() )
                        pool.push_back( std::make_shared<DataDrivenMaterial>( *cell ) );
                    DataDrivenMaterial& probe = *pool.front();
                    if ( IsTranslucent( &probe ) )
                        continue;
                    const bool ready = ownCell( m_StaticGBufferPipeline.get(), probe, probe.IsTwoSided() );
                    // Its identity: the cell, its textures and its values - an edited override recaptures its cards.
                    uint64_t surfaceKey = HashCombine( 0x9E3779B97F4A7C15ull, std::hash<std::string>{}( *cell ) );
                    surfaceKey          = HashCombine( surfaceKey, std::hash<std::string>{}( GenericTextureKey( g ) ) );
                    for ( const auto& [name, value] : g.Overrides().Params )
                    {
                        surfaceKey = HashCombine( surfaceKey, std::hash<std::string>{}( name ) );
                        for ( int lane = 0; lane < 4; ++lane )
                            surfaceKey = HashCombine( surfaceKey, std::hash<float>{}( value[lane] ) );
                    }
                    section.Generic      = &g;
                    section.OverrideCell = *cell;
                    addSection( entryKey, g.Mesh, cards, g.Transform, MeshVertexPath::Static, std::move( section ),
                                std::pair{ surfaceKey, ready } );
                }
                // Not followed by the stream: gathered again next frame.
                if ( !followed )
                    group.Gathered = false;
            }
            m_CardGroupOrder.push_back( &group );
        }

        // ISM FIELDS, one group each. Instanced static meshes and foliage: every instance is an instance of the
        // Lumen scene (UE adds each ISM instance's cards), keyed by its field's entity, its mesh and its index -
        // stable while the set is; a moved instance keeps its texels. An entry is one section group of the
        // field: instance i of every group is one card instance, drawn with each group's material. A field is
        // walked again only when its key (O(section groups), never O(instances)) or one of its transforms
        // snapshots moved.
        {
            const auto fieldOf = []( const InstancedMeshRenderData& ism )
            { return HashCombine( HashCombine( 0x84222325CBF29CE4ull, ism.Entity ), PointerBits( ism.Mesh ) ); };
            std::vector<uint64_t>                                  fieldOrder;
            std::unordered_map<uint64_t, std::vector<std::size_t>> fieldEntries;
            for ( std::size_t e = 0; e < m_InstancedQueue.size(); ++e )
            {
                const auto& ism = m_InstancedQueue[e];
                if ( ism.Mesh == nullptr || !ism.Transforms || ism.Transforms->empty() )
                    continue;
                auto& entries = fieldEntries[fieldOf( ism )];
                if ( entries.empty() )
                    fieldOrder.push_back( fieldOf( ism ) );
                entries.push_back( e );
            }
            for ( const uint64_t field : fieldOrder )
            {
                const std::vector<std::size_t>& entries = fieldEntries[field];
                CardGatherGroup&                group   = m_CardFieldGroups[field];
                group.Id                                = field | 0x8000000000000000ull;
                group.SeenGather                        = m_CardGather;
                uint64_t key                            = HashCombine( readiness, entries.size() );
                bool     same = group.Gathered && group.Snapshots.size() == entries.size();
                for ( std::size_t s = 0; s < entries.size(); ++s )
                {
                    const auto&          ism     = m_InstancedQueue[entries[s]];
                    const SectionShading shading = ShadingOf( ism );
                    key                          = HashCombine( key, PointerBits( ism.Mesh ) );
                    key                          = HashCombine( key, ism.Mesh->GetRenderRevision() );
                    key                          = HashCombine( key, PointerBits( shading.Surface ) );
                    key                          = HashCombine( key, PointerBits( shading.Instance ) );
                    key                          = HashCombine( key, ism.HiddenSubmeshes );
                    key                          = HashCombine( key, ism.Transforms->size() );
                    if ( same )
                    {
                        const auto& held = group.Snapshots[s];
                        same             = !held.owner_before( ism.Transforms ) && !ism.Transforms.owner_before( held );
                    }
                }
                if ( same && group.Key == key )
                {
                    m_StandInDraws += group.StandIns;
                    m_CardGroupOrder.push_back( &group );
                    continue;
                }
                startGroup( group, key );
                group.Snapshots.clear();
                for ( const std::size_t e : entries )
                {
                    const auto& ism = m_InstancedQueue[e];
                    group.Snapshots.push_back( ism.Transforms );
                    const SectionShading shading = ShadingOf( ism );
                    if ( shading.Surface == nullptr || IsTranslucent( shading.Surface ) )
                        continue;
                    const auto cards = ism.Mesh->GetCards();
                    if ( !cards || cards->Cards.empty() )
                        continue;
                    // The field's surface identity and readiness, asked once for all its instances (SCENE-CACHE-4):
                    // every instance is the same (surface, instance, path), so the answer is the same.
                    uint64_t fieldSurfaceKey = 0;
                    bool     fieldReady      = false;
                    surfaceOf( shading.Surface, shading.Instance, MeshVertexPath::Instanced, fieldSurfaceKey,
                               fieldReady );
                    const std::optional<std::pair<uint64_t, bool>> fieldSurface( std::in_place, fieldSurfaceKey,
                                                                                 fieldReady );
                    for ( std::size_t i = 0; i < ism.Transforms->size(); ++i )
                    {
                        // Bit 63 set: never an entity key (entity << 24 stays below it).
                        const uint64_t instanceKey = 0x8000000000000000ull | ( HashCombine( field, i ) >> 1u );
                        addSection( instanceKey, ism.Mesh, cards, ( *ism.Transforms )[i], MeshVertexPath::Instanced,
                                    { .Instance = shading.Instance, .Surface = shading.Surface, .Hidden = ism.HiddenSubmeshes },
                                    fieldSurface );
                    }
                }
                m_CardGroupOrder.push_back( &group );
            }
            // A field the queue no longer has: its group goes (the surface cache frees its instances).
            std::erase_if( m_CardFieldGroups,
                           [this]( const auto& entry ) { return entry.second.SeenGather != m_CardGather; } );
        }

        // THE SKINNED GROUP, every frame (posed, few). SKINNED MESHES (UE MeshCardRepresentation
        // SetCardsFromBounds): a skinned mesh has no card build; its cards are the six faces of its bind-pose box,
        // captured through its (Skinned x GBuffer) cell in the pose of the frame that captures them, each opaque
        // section (data.Sections) with its own material. Bit 62 set: never an entity key of the static or generic
        // queue.
        {
            CardGatherGroup& group = m_CardSkinnedGroup;
            group.Id               = 2u;
            startGroup( group, readiness );
            for ( const auto& data : m_SkinnedQueue )
            {
                if ( data.Mesh == nullptr )
                    continue;
                const uint64_t key = 0x4000000000000000ull | ( static_cast<uint64_t>( data.Entity ) << 24u );
                std::shared_ptr<const Assets::MeshCardsBuildData>* cards = nullptr;
                for ( const SkinnedSection& skinned : data.Sections )
                {
                    const SectionShading shading = ShadingOf( skinned );
                    if ( shading.Surface == nullptr || shading.Instance == nullptr ||
                         IsTranslucent( shading.Surface ) )
                        continue;
                    if ( cards == nullptr )
                    {
                        cards = &m_SkinnedBoundsCards[data.Mesh];
                        if ( !*cards )
                        {
                            const Common::Math::AABB box = Geometry::LocalBounds( data.Mesh->GetSubmeshes() );
                            if ( Geometry::IsEmpty( box ) )
                                break;
                            auto built       = std::make_shared<Assets::MeshCardsBuildData>();
                            built->BoundsMin = box.Min;
                            built->BoundsMax = box.Max;
                            for ( uint32_t direction = 0; direction < Assets::kCardDirections; ++direction )
                                built->Cards.push_back(
                                     { .Center    = 0.5f * ( box.Min + box.Max ),
                                       .Extent    = 0.5f * ( box.Max - box.Min ),
                                       .Direction = static_cast<Assets::CardDirection>( direction ) } );
                            *cards = std::move( built );
                        }
                    }
                    addSection( key, data.Mesh, *cards, data.Transform, MeshVertexPath::Skinned,
                                { .Instance     = shading.Instance,
                                  .Surface      = shading.Surface,
                                  .Hidden       = skinned.HiddenSubmeshes,
                                  .GpuPrimitive = data.GpuPrimitive },
                                std::nullopt );
                }
            }
            // Gathered every frame: its revision moved, but the surface cache finds the same instances where they
            // were and changes nothing for them.
            m_CardGroupOrder.push_back( &group );
        }

        out.reserve( m_CardGroupOrder.size() );
        for ( const CardGatherGroup* group : m_CardGroupOrder )
            out.push_back( { .Id = group->Id, .Revision = group->Revision, .Instances = group->Instances } );
    }

    void MeshRenderer::DeclareCardCaptureDraws( RDG::PassBuilder&                             pass,
                                                const std::vector<SurfaceCache::CardCapture>& captures,
                                                const SceneViewInputs&                        view )
    {
        m_CardCaptureDraws.Clear();
        if ( captures.empty() )
            return;
        auto* const materials = Runtime::ResourceRegistry::GetMaterialService();
        // Every section of the captured instance is one draw into the card's rectangle (UE FLumenCardRenderer:
        // a mesh batch per section, each with its own material). The k-th section draw of a surface in this
        // frame draws through that surface's k-th view copy (its camera block holds this card's camera); the
        // renderer-owned surface through the k-th material of its cell's pool; a Shader Override through the
        // k-th material of its cell's pool, its texture overrides bound. Every section reads its row from the scene
        // material table, lent to the material it records with.
        std::unordered_map<const DataDrivenMaterial*, uint32_t> viewsTaken;
        std::unordered_map<const void*, uint32_t>               overridesTaken;
        uint32_t                                                pooled[3] = { 0u, 0u, 0u };
        for ( const SurfaceCache::CardCapture& capture : captures )
        {
            const CardCaptureSource& source    = m_CardGroupOrder[capture.Group]->Sources[capture.Source];
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
            // The card is not a view of its own (no jitter, no previous frame): its camera, in each section's
            // material. The frame state also carries the view's ObjectMotions and ObjectBones, which place a
            // static or generic mesh and pose a skinned one by its motion row.
            const ViewFrame cardView =
                 MakeStillViewFrame( capture.View, capture.Projection, capture.Eye, m_WorldTimeSeconds );
            const SurfaceCache::AtlasRect rect = capture.CaptureRect;
            for ( const CardCaptureSection& section : source.Sections )
            {
                DataDrivenMaterial* material = nullptr;
                MaterialInstance*   instance = section.Instance;
                // The section's row in the scene material table, the one its scene draw names (SyncGpuScene
                // registered it this frame: the captured sections are this frame's queue records).
                std::optional<uint32_t> row;
                if ( section.Generic != nullptr )
                {
                    auto& pool =
                         m_CardCaptureOverrides[section.OverrideCell + GenericTextureKey( *section.Generic )];
                    const uint32_t k = overridesTaken[&pool]++;
                    while ( pool.size() <= k )
                        pool.push_back( std::make_shared<DataDrivenMaterial>( section.OverrideCell ) );
                    material = pool[k].get();
                    // Its descriptors (TextureBinding textures); its values are its record's table row.
                    ApplyGenericTextures( *material, *section.Generic, section.OverrideCell, true, 0 );
                    row = section.Generic->MaterialRow;
                }
                else if ( materials != nullptr && materials->Owns( section.Surface ) )
                {
                    const uint32_t view = viewsTaken[section.Surface]++;
                    material = materials->GetViewVariant( section.Surface, path, MeshPass::GBuffer, view );
                    if ( material == nullptr )
                    {
                        // Gathered as ready, so the cell exists: MaterialService named what refused the copy.
                        m_CardCaptureDraws.Fail(
                             std::format( "the ({} x GBuffer) view copy {} of a surface for the card "
                                          "capture was refused",
                                          MeshVertexPathName( path ), view ) );
                        return;
                    }
                    row = FindMaterialRow( section.Surface, section.Instance );
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
                    instance = section.Instance != nullptr ? section.Instance : instances[pooled[cell]].get();
                    ++pooled[cell];
                    row = FindMaterialRow( section.Surface, section.Instance );
                }
                // A miss was said by FindMaterialRow (or SyncGpuScene could not shade the generic record): the
                // section is not drawn into the card rather than drawn with another object's row.
                if ( !row || !LendMaterialTable( *material ) )
                    continue;
                // An instance's one transform (the instanced cell reads its world from InstanceTransforms).
                if ( path == MeshVertexPath::Instanced )
                    if ( auto* sb = material->Get<StorageBufferProperty>(
                              MeshCardCaptureShaderParameters::InstanceTransforms ) )
                        sb->SetRawData( &source.World, static_cast<uint32_t>( sizeof( glm::mat4 ) ) );
                CaptureFrameState( &cardView ).ApplyTo( static_cast<Material*>( material ) );

                const bool twoSided = instance != nullptr ? instance->IsTwoSided() : material->IsTwoSided();
                GraphicsPipeline* const pipeline =
                     CullPermutation( CellPipeline( passState, *material ), twoSided );
                // The view-pass cells place the mesh themselves (static, generic, skinned: the ObjectMotions row;
                // instanced: the instance transform): the push transform is the submesh's alone, as in the
                // scene's G-buffer draw. The section's mask leaves the other sections' submeshes to their draws.
                m_CardCaptureDraws.Add( { .Pipeline          = pipeline,
                                          .Mesh              = source.Mesh,
                                          .Transform         = glm::mat4( 1.0f ),
                                          .Material          = material->GetMaterialExecutor(),
                                          .InstanceCount     = 1u,
                                          .FirstInstance     = 0u,
                                          .HiddenSubmeshMask = section.Hidden,
                                          .Push =
                                               [&]
                                          {
                                              DrawPushFields push;
                                              push.Transform( glm::mat4( 1.0f ) ).MaterialIndex( *row );
                                              if ( path == MeshVertexPath::Instanced )
                                                  push.Wind( InstanceWindPush{} ); // a still card
                                              else
                                                  push.PrimitiveIndex( section.GpuPrimitive );
                                              return push;
                                          }(),
                                          // Its own rectangle of the capture atlas only (the pass cleared the
                                          // whole atlas: every capture of the frame is new).
                                          .Viewport = MeshRendererDetail::MeshDrawCommand::ViewportRegion{
                                               .X      = static_cast<int32_t>( rect.X ),
                                               .Y      = static_cast<int32_t>( rect.Y ),
                                               .Width  = rect.Width,
                                               .Height = rect.Height } } );
            }
        }
        m_CardCaptureDraws.Declare( pass, view );
    }

    Common::BoolResultStr MeshRenderer::RenderCardCaptureManual( const RDG::PassContext& context ) const
    {
        return m_CardCaptureDraws.Record( context );
    }
} // namespace Desert::Graphic::System
