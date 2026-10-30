// Ported from UE 5.8 Engine/Source/Runtime/Renderer/Private/GlobalDistanceField.cpp (clipmap extents :488-499 with
// Lumen's 25 m first extent, the four-voxel influence radius :464, partial updates of the regions a camera move
// exposes and of the bounds of changed primitives), adapted: dense toroidal RG16F clipmaps (distance, coverage)
// instead of the page atlas and its coverage atlas, objects culled to update bricks on the CPU, one compute pass
// per level that has bricks to rewrite.
#include <Engine/Graphic/DistanceField/GlobalDistanceField.hpp>
#include <Engine/Core/EngineContext.hpp>

#include <Engine/Geometry/MeshBounds.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/ShaderResources/StorageBuffer.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <span>
#include <tuple>
#include <unordered_set>
#include <utility>

namespace Desert::Graphic::DistanceField
{
    namespace DFShared
    {
#include <Common/DistanceFieldShared.glslh>
    } // namespace DFShared

    static_assert( DFShared::GDF_NUM_CLIPMAPS == kGlobalDistanceFieldClipmaps );

    namespace
    {
        constexpr int      kGdfResolution    = DFShared::GDF_RESOLUTION;
        constexpr int      kGdfBrick         = DFShared::GDF_UPDATE_BRICK;
        constexpr int      kGdfBricksPerSide = kGdfResolution / kGdfBrick;
        constexpr int      kGdfHalfBricks    = kGdfBricksPerSide / 2;
        constexpr uint32_t kGdfObjectStride  = static_cast<uint32_t>( DFShared::DF_OBJECT_STRIDE );
        static_assert( kGdfResolution % ( 2 * kGdfBrick ) == 0 );

        // Buffer bindings of GlobalDistanceFieldComposite.shader.
        constexpr uint32_t kGdfAssetBinding       = kMeshDistanceFieldAssetBinding;
        constexpr uint32_t kGdfIndirectionBinding = kMeshDistanceFieldIndirectionBinding;
        constexpr uint32_t kGdfObjectBinding      = 3;
        constexpr uint32_t kGdfUpdateBinding      = 4;
        constexpr uint32_t kGdfBrickListBinding   = 5;

        constexpr int kGdfGridCells = DFShared::GDF_OBJECT_GRID_RESOLUTION;
        constexpr int kGdfGridSlots = DFShared::GDF_OBJECT_GRID_SLOTS;
        static_assert( kGdfGridCells * DFShared::GDF_OBJECT_GRID_CELL_VOXELS == kGdfResolution );
        static_assert( kGdfBrick % DFShared::GDF_OBJECT_GRID_CELL_VOXELS == 0, "a brick holds whole cells" );

        struct GdfCompositePush
        {
            uint32_t FirstBrick      = 0;
            float    VoxelSize       = 0.0f;
            float    InfluenceRadius = 0.0f;
            uint32_t Level           = 0; // the object grid's slab (GlobalDistanceField.hpp)
        };
        struct GdfViewPush
        {
            glm::mat4 InverseViewProjection;
            glm::vec4 Levels[kGlobalDistanceFieldClipmaps];
        };
        static_assert( sizeof( GdfViewPush ) == 128, "the engine's push block cap" );
        // The object record's rows as AddUpdatePasses writes them: the world box, then the flags last.
        static_assert( DFShared::DF_OBJECT_STRIDE == 11 && DFShared::DF_OBJECT_ROW_BOUNDS_MIN == 5 &&
                       DFShared::DF_OBJECT_ROW_BOUNDS_MAX == 6 && DFShared::DF_OBJECT_ROW_FLAGS == 7 &&
                       DFShared::DF_OBJECT_ROW_TO_PREVIOUS_WORLD == 8 );

        // std430 `struct MeshSDFCullParams` of Common/MeshSDFGrid.glslh.
        struct MeshSDFCullPayload
        {
            MeshSDFGridPlacement     Grid;
            glm::mat4                ClipToWorld{ 1.0f };
            glm::vec4                ClipDepth{ 0.0f };
            std::array<glm::vec4, 6> Planes{};
            glm::uvec4               Counts{ 0u };
        };
        static_assert( sizeof( MeshSDFCullPayload ) == 304 );
        // Common/MeshSDFGrid.glslh: MeshSDFGridHeaderStride, MeshSDFCullFeedbackUints.
        constexpr uint32_t kGridHeaderStride  = 2u;
        constexpr uint32_t kCullFeedbackUints = 4u;
        // MeshSDFCullGrid.shader's push block: 0 = count, 1 = scatter.
        struct MeshSDFCullGridPush
        {
            uint32_t Pass = 0;
        };

        // The world plane of clip-space half-space row3 + sign * row(axis) >= 0, normalised.
        glm::vec4 ClipPlane( const glm::mat4& viewProjection, const int axis, const float sign )
        {
            const glm::vec4 row3( viewProjection[0][3], viewProjection[1][3], viewProjection[2][3],
                                  viewProjection[3][3] );
            const glm::vec4 row( viewProjection[0][axis], viewProjection[1][axis], viewProjection[2][axis],
                                 viewProjection[3][axis] );
            const glm::vec4 plane = row3 + sign * row;
            return plane / glm::length( glm::vec3( plane ) );
        }

        float GdfInfluenceRadius( const float voxelSize )
        {
            return DFShared::GDF_INFLUENCE_RANGE_IN_VOXELS * voxelSize;
        }

        // The integer brick range [lo, hi] a world box touches on a level.
        void GdfBrickRange( const Common::Math::AABB& box, const float voxelSize, glm::ivec3& lo, glm::ivec3& hi )
        {
            const float brickSize = voxelSize * static_cast<float>( kGdfBrick );
            lo                    = glm::ivec3( glm::floor( box.Min / brickSize ) );
            hi                    = glm::ivec3( glm::floor( box.Max / brickSize ) );
        }

        // The identity of a placed field: its address and its transform's bits.
        uint64_t GdfObjectKey( const GlobalDistanceFieldObject& object )
        {
            uint64_t     hash = std::bit_cast<uint64_t>( object.Field.get() ) * 0x9E3779B97F4A7C15ULL;
            const float* m    = &object.World[0][0];
            for ( int i = 0; i < 16; ++i )
                hash = ( hash ^ std::bit_cast<uint32_t>( m[i] ) ) * 0x100000001B3ULL;
            return ( hash ^ ( object.MostlyTwoSided ? 1u : 0u ) ) * 0x100000001B3ULL;
        }

        template <typename T>
        Common::BoolResultStr GdfUpload( ShaderResources::StorageBuffer& buffer, const std::vector<T>& data,
                                      const std::string_view what )
        {
            // A zero-sized write has nowhere to go; an empty list is uploaded as one zero element nobody reads.
            static const T zero{};
            const void*    bytes = data.empty() ? static_cast<const void*>( &zero ) : data.data();
            const auto     size  = static_cast<uint32_t>( std::max<size_t>( 1, data.size() ) * sizeof( T ) );
            if ( const auto wrote = buffer.SetData( bytes, size ); !wrote )
                return Common::MakeFormattedError<bool>( "global distance field: {} upload ({} bytes): {}", what,
                                                         size, wrote.GetError() );
            return BOOLSUCCESS;
        }
        // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
        struct MeshSDFCullShaderParameters
        {
            DESERT_SHADER_PARAMETER( StorageBuffer, MeshSDFCullParamsUB );
            DESERT_SHADER_PARAMETER( StorageBuffer, DFObjects );
            DESERT_SHADER_PARAMETER( StorageBuffer, DFAssetData );
            DESERT_SHADER_PARAMETER( StorageBuffer, MeshSDFCulledObjectsUB );
        };

        // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
        struct MeshSDFGridShaderParameters
        {
            DESERT_SHADER_PARAMETER( StorageBuffer, MeshSDFCullParamsUB );
            DESERT_SHADER_PARAMETER( StorageBuffer, DFObjects );
            DESERT_SHADER_PARAMETER( StorageBuffer, MeshSDFCulledObjectsUB );
            DESERT_SHADER_PARAMETER( StorageBuffer, MeshSDFGridCellsUB );
            DESERT_SHADER_PARAMETER( StorageBuffer, MeshSDFGridUnsortedUB );
        };

        // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
        struct MeshSDFGridOffsetsShaderParameters
        {
            DESERT_SHADER_PARAMETER( StorageBuffer, MeshSDFCullParamsUB );
            DESERT_SHADER_PARAMETER( StorageBuffer, MeshSDFGridCellsUB );
            DESERT_SHADER_PARAMETER( StorageBuffer, MeshSDFCullFeedbackUB );
        };

        // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
        struct MeshSDFGridSortShaderParameters
        {
            DESERT_SHADER_PARAMETER( StorageBuffer, MeshSDFCullParamsUB );
            DESERT_SHADER_PARAMETER( StorageBuffer, MeshSDFGridCellsUB );
            DESERT_SHADER_PARAMETER( StorageBuffer, MeshSDFGridUnsortedUB );
        };

    } // namespace

    float ClipmapVoxelSize( const int level )
    {
        return 2.0f * DFShared::GDF_CLIPMAP0_EXTENT * static_cast<float>( 1 << level ) /
               static_cast<float>( kGdfResolution );
    }

    glm::ivec3 ClipmapCentreBrick( const glm::dvec3& camera, const float voxelSize )
    {
        return glm::ivec3( glm::round( camera / ( static_cast<double>( voxelSize ) * kGdfBrick ) ) );
    }

    std::vector<glm::ivec3> PlanClipmapUpdate( const std::optional<glm::ivec3>& previousCentre,
                                               const glm::ivec3& centre, const float voxelSize,
                                               const std::vector<Common::Math::AABB>& dirty )
    {
        const glm::ivec3        lo = centre - kGdfHalfBricks;
        const glm::ivec3        hi = centre + kGdfHalfBricks; // exclusive
        std::vector<glm::ivec3> bricks;
        const bool              whole =
             !previousCentre.has_value() || glm::any( glm::greaterThanEqual( glm::abs( centre - *previousCentre ),
                                                                             glm::ivec3( kGdfBricksPerSide ) ) );
        if ( whole )
        {
            bricks.reserve( static_cast<size_t>( kGdfBricksPerSide ) * kGdfBricksPerSide * kGdfBricksPerSide );
            for ( int z = lo.z; z < hi.z; ++z )
                for ( int y = lo.y; y < hi.y; ++y )
                    for ( int x = lo.x; x < hi.x; ++x )
                        bricks.emplace_back( x, y, z );
            return bricks;
        }
        // Entered the window: outside the previous one.
        const glm::ivec3             plo = *previousCentre - kGdfHalfBricks;
        const glm::ivec3             phi = *previousCentre + kGdfHalfBricks;
        std::unordered_set<uint64_t> taken;
        const auto                   key = [&lo]( const glm::ivec3& b )
        {
            const glm::uvec3 o( b - lo );
            return ( static_cast<uint64_t>( o.z ) * kGdfBricksPerSide + o.y ) * kGdfBricksPerSide + o.x;
        };
        for ( int z = lo.z; z < hi.z; ++z )
            for ( int y = lo.y; y < hi.y; ++y )
                for ( int x = lo.x; x < hi.x; ++x )
                {
                    const glm::ivec3 b( x, y, z );
                    if ( glm::all( glm::greaterThanEqual( b, plo ) ) && glm::all( glm::lessThan( b, phi ) ) )
                        continue;
                    taken.insert( key( b ) );
                    bricks.push_back( b );
                }
        // Touched by a change.
        for ( const auto& box : dirty )
        {
            glm::ivec3 blo, bhi;
            GdfBrickRange( box, voxelSize, blo, bhi );
            blo = glm::max( blo, lo );
            bhi = glm::min( bhi, hi - 1 );
            for ( int z = blo.z; z <= bhi.z; ++z )
                for ( int y = blo.y; y <= bhi.y; ++y )
                    for ( int x = blo.x; x <= bhi.x; ++x )
                    {
                        const glm::ivec3 b( x, y, z );
                        if ( taken.insert( key( b ) ).second )
                            bricks.push_back( b );
                    }
        }
        return bricks;
    }

    GlobalDistanceField::GlobalDistanceField( const ResourceOwnerInstance owner ) : m_LedgerInstance( owner ) {}
    GlobalDistanceField::~GlobalDistanceField() = default;

    Common::BoolResultStr GlobalDistanceField::Initialize()
    {
        // The owner's rows, whichever view's scope is open (a capture's field is the scene's, not its first face's).
        const ResourceAttributionScope ownerRows( ResourceOwner::SceneRenderer, m_LedgerInstance );
        const auto shaders = Runtime::ResourceRegistry::GetShaderService();
        m_CompositeShader  = shaders->GetByName( "GlobalDistanceFieldComposite" );
        if ( !m_CompositeShader )
            return Common::MakeError<bool>( "global distance field: shader GlobalDistanceFieldComposite not found" );
        auto pipeline = ComputePipeline::Create(
             { .Shader = m_CompositeShader, .DebugName = "GlobalDistanceFieldComposite" } );
        if ( !pipeline )
            return Common::MakeError<bool>( pipeline.GetError() );
        for ( int level = 0; level < kGlobalDistanceFieldClipmaps; ++level )
        {
            const Core::Formats::Image3DSpecification spec{
                 .Tag        = std::format( "GlobalDistanceField.Clipmap{}", level ),
                 .Width      = static_cast<uint32_t>( kGdfResolution ),
                 .Height     = static_cast<uint32_t>( kGdfResolution ),
                 .Depth      = static_cast<uint32_t>( kGdfResolution ),
                 .Format     = Core::Formats::ImageFormat::RG16F,
                 .Data       = Core::Formats::EmptyPixelData{},
                 .Properties = Core::Formats::Storage | Core::Formats::Sample,
            };
            m_Clipmaps[level] = Image3D::Create( spec );
            if ( !m_Clipmaps[level] )
                return Common::MakeFormattedError<bool>(
                     "global distance field: clipmap {} ({}^3 RG16F) not created", level, kGdfResolution );
        }
        // The object grid: every level's cells in one integer volume (GlobalDistanceField.hpp, THE OBJECT GRID).
        {
            const Core::Formats::Image3DSpecification spec{
                 .Tag        = "GlobalDistanceField.ObjectGrid",
                 .Width      = static_cast<uint32_t>( kGdfGridCells * kGdfGridSlots ),
                 .Height     = static_cast<uint32_t>( kGdfGridCells ),
                 .Depth      = static_cast<uint32_t>( kGdfGridCells * kGlobalDistanceFieldClipmaps ),
                 .Format     = Core::Formats::ImageFormat::R32_UINT,
                 .Data       = Core::Formats::EmptyPixelData{},
                 .Properties = Core::Formats::Storage | Core::Formats::Sample,
            };
            m_ObjectGrid = Image3D::Create( spec );
            if ( !m_ObjectGrid )
                return Common::MakeFormattedError<bool>(
                     "global distance field: the object grid ({}x{}x{} R32_UINT) "
                     "was not created",
                     spec.Width, spec.Height, spec.Depth );
        }
        // Per-frame buffers (persistent = false): each frame in flight writes its own copy. The update bricks and
        // their lists are written by a frame that dispatches a composite; the fields and the objects every frame,
        // since the near field's cull and the GI trace read them every frame.
        m_ObjectBuffer = ShaderResources::StorageBuffer::Create( "DFObjects", 16, kGdfObjectBinding, false );
        m_UpdateBrickBuffer =
             ShaderResources::StorageBuffer::Create( "GDFUpdateBricks", 16, kGdfUpdateBinding, false );
        m_BrickObjectBuffer =
             ShaderResources::StorageBuffer::Create( "GDFBrickObjects", 16, kGdfBrickListBinding, false );
        if ( !m_ObjectBuffer || !m_UpdateBrickBuffer || !m_BrickObjectBuffer )
            return Common::MakeError<bool>( "global distance field: a storage buffer could not be created" );
        if ( const auto valid = m_CompositeLayout.Validate( m_CompositeShader, GlobalDistanceFieldCompositeShaderParameters::kAll );
             !valid )
            return valid;
        // Last: IsInitialized() reads the composite pipeline, so a field is initialised only once everything is.
        m_CompositePipeline = pipeline.GetValue();
        return BOOLSUCCESS;
    }

    GlobalDistanceFieldView::GlobalDistanceFieldView()  = default;
    GlobalDistanceFieldView::~GlobalDistanceFieldView() = default;

    Common::BoolResultStr GlobalDistanceFieldView::Initialize()
    {
        const auto shaders = Runtime::ResourceRegistry::GetShaderService();
        m_ViewShader       = shaders->GetByName( "GlobalDistanceFieldView" );
        if ( !m_ViewShader )
            return Common::MakeError<bool>( "global distance field view: shader GlobalDistanceFieldView not found" );
        if ( const auto valid = m_ViewLayout.Validate( m_ViewShader, GlobalDistanceFieldViewShaderParameters::kAll );
             !valid )
            return valid;
        for ( const auto& [name, shader, cullPipeline] :
              { std::tuple{ "MeshSDFCullObjects", &m_CullObjectsShader, &m_CullObjectsPipeline },
                std::tuple{ "MeshSDFCullGrid", &m_CullGridShader, &m_CullGridPipeline },
                std::tuple{ "MeshSDFCullGridOffsets", &m_CullOffsetsShader, &m_CullOffsetsPipeline },
                std::tuple{ "MeshSDFCullGridSort", &m_CullSortShader, &m_CullSortPipeline } } )
        {
            *shader = shaders->GetByName( name );
            if ( !*shader )
                return Common::MakeFormattedError<bool>( "global distance field view: shader {} not found", name );
            auto created = ComputePipeline::Create( { .Shader = *shader, .DebugName = name } );
            if ( !created )
                return Common::MakeError<bool>( created.GetError() );
            *cullPipeline = created.GetValue();
        }

        return BOOLSUCCESS;
    }

    Common::ResultStr<GlobalDistanceField::FrameField>
    GlobalDistanceField::AddUpdatePasses( RDG::Builder& graph, const ImportImage& import, const glm::vec3& camera,
                                          const std::vector<GlobalDistanceFieldObject>& objects,
                                          DistanceFieldSceneData& scene, const uint64_t engineFrame,
                                          const uint32_t poolMiB )
    {
        // The owner's rows: the per-frame buffers grow on upload (GdfUpload) inside the asking view's scope.
        const ResourceAttributionScope ownerRows( ResourceOwner::SceneRenderer, m_LedgerInstance );
        // What this call asks for: the camera and the object list's identity (field, transform, foliage flag and
        // row of each, in order) - the once-per-frame rule compares it with the frame's update.
        uint64_t inputs = 0xCBF29CE484222325ULL ^ static_cast<uint64_t>( objects.size() );
        for ( const auto& object : objects )
        {
            inputs = ( inputs ^ GdfObjectKey( object ) ) * 0x100000001B3ULL;
            inputs = ( inputs ^ ( static_cast<uint64_t>( object.Row ) + 1u ) ) * 0x100000001B3ULL;
        }
        if ( m_FrameUpdate && m_FrameUpdate->Frame == engineFrame )
        {
            if ( camera != m_FrameUpdate->Camera || inputs != m_FrameUpdate->Objects )
                return Common::MakeFormattedError<FrameField>(
                     "global distance field: updated once per frame - frame {} was composited for another camera "
                     "or object set (its per-frame buffers are what that frame's earlier graph reads)",
                     engineFrame );
            // The frame's update already ran: this graph reads the same field, nothing is recorded.
            FrameField again;
            for ( int level = 0; level < kGlobalDistanceFieldClipmaps; ++level )
                again.Clipmaps[level] =
                     import( m_Clipmaps[level], std::format( "GlobalDistanceField.Clipmap{}", level ) );
            again.ObjectGrid            = import( m_ObjectGrid, "GlobalDistanceField.ObjectGrid" );
            again.Levels                = m_FrameUpdate->Levels;
            again.MeshFields.NumObjects = m_FrameUpdate->NumObjects;
            again.MeshFields.BrickAtlas = import( scene.GetBrickAtlas(), "DistanceField.BrickAtlas" );
            return Common::MakeSuccess( again );
        }

        // The scene's frame: the first view of it decides the scene's resident fields, every later view finds them as
        // that one left them (DistanceFieldSceneData::BeginViewFrame).
        // The fields of every distance-field primitive of the scene (the list is the scene's, not culled).
        std::vector<std::shared_ptr<const Assets::MeshDistanceFieldVolume>> fields;
        fields.reserve( objects.size() );
        for ( const auto& object : objects )
            fields.push_back( object.Field );
        if ( const auto begun = scene.BeginViewFrame( engineFrame, poolMiB, fields ); !begun )
            return Common::MakeError<FrameField>( begun.GetError() );
        // A re-created pool dropped every resident field: everything this view composited from the old atlas is
        // rewritten.
        if ( m_PoolGeneration != scene.GetPoolGeneration() )
        {
            m_PoolGeneration = scene.GetPoolGeneration();
            m_ValidCentre.fill( std::nullopt );
            m_Tracked.clear();
        }

        // Each object's record in the frame's resident set (its distance from this camera merges into the
        // scene's next streaming); an object whose field the set lacks waits for the next frame.
        struct Placed
        {
            const GlobalDistanceFieldObject* Object;
            uint32_t                         AssetIndex;
            Common::Math::AABB               Bounds;
            uint64_t                         Key;
        };
        std::vector<Placed> placed;
        placed.reserve( objects.size() );
        for ( const auto& object : objects )
        {
            const Common::Math::AABB bounds =
                 Geometry::TransformBounds( object.World, object.Field->LocalSpaceMeshBounds );
            // The largest per-axis gap between the camera and the box: the camera-centred clipmap box of that
            // half-extent is the smallest one the object overlaps (UE ComputeSquaredDistanceBetweenAABBs <= 0).
            const glm::vec3 gap   = glm::abs( glm::clamp( camera, bounds.Min, bounds.Max ) - camera );
            const std::optional<uint32_t> index =
                 scene.Lookup( object.Field, std::max( { gap.x, gap.y, gap.z } ) );
            if ( !index )
                continue;
            placed.push_back( { &object, *index, bounds, 0 } );
        }
        // An object is its field, its transform, how many of the field's mips are resident and the row the object
        // grid lists it under: a mip streaming in or out, or the instance taking another row, recomposites the
        // object like a move does.
        for ( auto& p : placed )
            p.Key = GdfObjectKey( *p.Object ) ^
                    ( static_cast<uint64_t>( scene.ResidentMips( p.Object->Field.get() ) ) *
                      0xC2B2AE3D27D4EB4FULL ) ^
                    ( ( static_cast<uint64_t>( p.Object->Row ) + 1u ) * 0x165667B19E3779F9ULL );

        // What changed since the clipmaps were written: objects that left and objects that arrived.
        std::vector<Common::Math::AABB>             changed;
        std::unordered_map<uint64_t, TrackedObject> current;
        for ( const auto& p : placed )
        {
            auto& tracked  = current[p.Key];
            tracked.Bounds = p.Bounds;
            ++tracked.Count;
        }
        for ( const auto& [key, tracked] : current )
            if ( const auto it = m_Tracked.find( key );
                 it == m_Tracked.end() || it->second.Count != tracked.Count )
                changed.push_back( tracked.Bounds );
        for ( const auto& [key, tracked] : m_Tracked )
            if ( !current.contains( key ) )
                changed.push_back( tracked.Bounds );
        m_Tracked = std::move( current );

        // The object buffer: world -> volume rows, the volume extent with the distance scale, the asset record,
        // the world box, the flags, the world -> previous world rows.
        std::vector<glm::vec4> objectData;
        objectData.reserve( placed.size() * kGdfObjectStride );
        for ( const auto& p : placed )
        {
            const auto&     field         = *p.Object->Field;
            const float     localToVolume = field.LocalToVolumeScale();
            const glm::vec3 centre = ( field.LocalSpaceMeshBounds.Min + field.LocalSpaceMeshBounds.Max ) * 0.5f;
            const glm::mat4 worldToVolume = glm::scale( glm::mat4( 1.0f ), glm::vec3( localToVolume ) ) *
                                            glm::translate( glm::mat4( 1.0f ), -centre ) *
                                            glm::inverse( p.Object->World );
            const glm::mat4 rows = glm::transpose( worldToVolume );
            // The smallest axis scale keeps the world distance conservative under a non-uniform scale (UE
            // VolumeToWorldScale uses the minimum scale as well).
            const glm::mat3 linear( p.Object->World );
            const glm::vec3 axisScale( glm::length( linear[0] ), glm::length( linear[1] ),
                                       glm::length( linear[2] ) );
            const float     minScale = std::min( { axisScale.x, axisScale.y, axisScale.z } );
            objectData.push_back( rows[0] );
            objectData.push_back( rows[1] );
            objectData.push_back( rows[2] );
            objectData.push_back( glm::vec4( field.VolumePositionExtent(), minScale / localToVolume ) );
            // Each volume axis' own scale: the box distance (MeshDistanceField.glslh) is exact, not the
            // smallest's.
            objectData.push_back( glm::vec4( std::bit_cast<float>( p.AssetIndex ), axisScale / localToVolume ) );
            objectData.push_back( glm::vec4( p.Bounds.Min, 0.0f ) ); // DF_OBJECT_ROW_BOUNDS_MIN
            objectData.push_back( glm::vec4( p.Bounds.Max, 0.0f ) ); // DF_OBJECT_ROW_BOUNDS_MAX
            const uint32_t flags = p.Object->MostlyTwoSided
                                        ? static_cast<uint32_t>( DFShared::DF_OBJECT_FLAG_MOSTLY_TWO_SIDED )
                                        : 0u;
            // DF_OBJECT_ROW_FLAGS: the flags, then the instance-table row - what the object grid lists and what a
            // near-field hit reads its cards by (one record for the composite and the trace).
            objectData.push_back(
                 glm::vec4( std::bit_cast<float>( flags ), std::bit_cast<float>( p.Object->Row ), 0.0f, 0.0f ) );
            // DF_OBJECT_ROW_TO_PREVIOUS_WORLD: this frame's world point on the object -> where it was last frame.
            const glm::mat4 toPrevious = glm::transpose( p.Object->PrevWorld * glm::inverse( p.Object->World ) );
            objectData.push_back( toPrevious[0] );
            objectData.push_back( toPrevious[1] );
            objectData.push_back( toPrevious[2] );
        }

        FrameField frame;
        frame.MeshFields.NumObjects = static_cast<uint32_t>( placed.size() );

        // The plan of every level, and each update brick's object list (CPU cull, objects grown by the influence).
        std::vector<glm::ivec4>                                                 updateBricks;
        std::vector<uint32_t>                                                   brickObjects;
        std::array<std::pair<uint32_t, uint32_t>, kGlobalDistanceFieldClipmaps> levelRange{};
        for ( int level = 0; level < kGlobalDistanceFieldClipmaps; ++level )
        {
            const float      voxel     = ClipmapVoxelSize( level );
            const float      influence = GdfInfluenceRadius( voxel );
            const glm::ivec3 centre    = ClipmapCentreBrick( glm::dvec3( camera ), voxel );
            frame.Levels[level]        = glm::vec4( glm::vec3( centre ) * ( voxel * kGdfBrick ), voxel );

            std::vector<Common::Math::AABB> dirty;
            dirty.reserve( changed.size() );
            for ( const auto& box : changed )
                dirty.push_back( { box.Min - influence, box.Max + influence } );
            const std::vector<glm::ivec3> plan = PlanClipmapUpdate( m_ValidCentre[level], centre, voxel, dirty );
            m_ValidCentre[level]               = centre;
            levelRange[level]                  = { static_cast<uint32_t>( updateBricks.size() ),
                                                   static_cast<uint32_t>( plan.size() ) };
            if ( plan.empty() )
                continue;

            // Brick -> its slot in this level's plan, then every object adds itself to the bricks it reaches.
            std::unordered_map<uint64_t, uint32_t> slotOf;
            slotOf.reserve( plan.size() );
            const glm::ivec3 lo    = centre - kGdfHalfBricks;
            const auto       keyOf = [&lo]( const glm::ivec3& b )
            {
                const glm::uvec3 o( b - lo );
                return ( static_cast<uint64_t>( o.z ) * kGdfBricksPerSide + o.y ) * kGdfBricksPerSide + o.x;
            };
            for ( uint32_t i = 0; i < plan.size(); ++i )
                slotOf.emplace( keyOf( plan[i] ), i );
            std::vector<std::vector<uint32_t>> lists( plan.size() );
            for ( uint32_t o = 0; o < placed.size(); ++o )
            {
                const Common::Math::AABB grown{ placed[o].Bounds.Min - influence,
                                                placed[o].Bounds.Max + influence };
                glm::ivec3               blo, bhi;
                GdfBrickRange( grown, voxel, blo, bhi );
                blo = glm::max( blo, lo );
                bhi = glm::min( bhi, centre + kGdfHalfBricks - 1 );
                for ( int z = blo.z; z <= bhi.z; ++z )
                    for ( int y = blo.y; y <= bhi.y; ++y )
                        for ( int x = blo.x; x <= bhi.x; ++x )
                            if ( const auto it = slotOf.find( keyOf( glm::ivec3( x, y, z ) ) );
                                 it != slotOf.end() )
                                lists[it->second].push_back( o );
            }
            for ( uint32_t i = 0; i < plan.size(); ++i )
            {
                updateBricks.emplace_back( plan[i], static_cast<int>( brickObjects.size() ) );
                brickObjects.push_back( static_cast<uint32_t>( lists[i].size() ) );
                brickObjects.insert( brickObjects.end(), lists[i].begin(), lists[i].end() );
            }
        }

        for ( int level = 0; level < kGlobalDistanceFieldClipmaps; ++level )
            frame.Clipmaps[level] =
                 import( m_Clipmaps[level], std::format( "GlobalDistanceField.Clipmap{}", level ) );
        frame.ObjectGrid = import( m_ObjectGrid, "GlobalDistanceField.ObjectGrid" );
        // What the near field's cull and a trace read, every frame: the fields, the objects. A view registers the
        // buffers in its own graph (GlobalDistanceFieldView::AddFramePasses).
        if ( auto r = GdfUpload( *m_ObjectBuffer, objectData, "objects" ); !r )
            return Common::MakeError<FrameField>( r.GetError() );
        const RDG::TextureRef atlas = import( scene.GetBrickAtlas(), "DistanceField.BrickAtlas" );
        frame.MeshFields.BrickAtlas = atlas;
        // The frame is updated from here on, also when it has no bricks to rewrite: the object buffer holds it.
        m_FrameUpdate = FrameUpdate{ .Frame      = engineFrame,
                                     .Camera     = camera,
                                     .Objects    = inputs,
                                     .Levels     = frame.Levels,
                                     .NumObjects = frame.MeshFields.NumObjects };
        if ( updateBricks.empty() )
            return Common::MakeSuccess( frame );

        if ( auto r = GdfUpload( *m_UpdateBrickBuffer, updateBricks, "update bricks" ); !r )
            return Common::MakeError<FrameField>( r.GetError() );
        if ( auto r = GdfUpload( *m_BrickObjectBuffer, brickObjects, "brick object lists" ); !r )
            return Common::MakeError<FrameField>( r.GetError() );
        // ASYNC COMPUTE (UE distance-field scene update on the async pipe): the composite reads the brick atlas (a
        // graph import) and buffers the host wrote before the graph ran, and writes only the clipmaps and the object grid;
        // it overlaps the shadow and base passes until the first graphics reader of the field joins it.
        const RDG::PassFlags kAsyncPass = RDG::PassFlags::Compute | RDG::PassFlags::AsyncCompute;
        for ( int level = 0; level < kGlobalDistanceFieldClipmaps; ++level )
        {
            const auto [first, count] = levelRange[level];
            if ( count == 0 )
                continue;
            GdfCompositePush push;
            push.FirstBrick                = first;
            push.VoxelSize                 = ClipmapVoxelSize( level );
            push.InfluenceRadius           = GdfInfluenceRadius( push.VoxelSize );
            push.Level                     = static_cast<uint32_t>( level );
            ComputePipeline*      pipeline = m_CompositePipeline.get();
            const RDG::TextureRef clipmap  = frame.Clipmaps[level];
            const RDG::TextureRef grid     = frame.ObjectGrid;
            graph.AddPass(
                 std::format( "GlobalDistanceField: Composite L{} ({} bricks)", level, count ),
                 kAsyncPass,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pipeline->SetStorageBuffer( kGdfAssetBinding, scene.GetAssetBuffer().get() );
                     pipeline->SetStorageBuffer( kGdfIndirectionBinding, scene.GetIndirectionBuffer().get() );
                     pipeline->SetStorageBuffer( kGdfObjectBinding, m_ObjectBuffer.get() );
                     pipeline->SetStorageBuffer( kGdfUpdateBinding, m_UpdateBrickBuffer.get() );
                     pipeline->SetStorageBuffer( kGdfBrickListBinding, m_BrickObjectBuffer.get() );
                     pass.Bindings( m_CompositeLayout.Get( m_CompositeShader ),
                                    Renderer::GetPipelineRouteFill( *pipeline ) )
                          .Texture( GlobalDistanceFieldCompositeShaderParameters::DistanceFieldBrickAtlasTexture, atlas, RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::All() )
                          .Sampler( GlobalDistanceFieldCompositeShaderParameters::LinearRepeatSampler, RDG::SamplerDesc::LinearRepeat() )
                          .Storage( GlobalDistanceFieldCompositeShaderParameters::ClipmapTexture, clipmap, RDG::Access::StorageWrite )
                          .Storage( GlobalDistanceFieldCompositeShaderParameters::ObjectGridTexture, grid, RDG::Access::StorageWrite )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( GdfCompositePush ) ) );
                 },
                 [pipeline, push, count]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                     return Renderer::DispatchCompute( bindings, *pipeline, count, 1, 1 );
                 } );
        }
        return Common::MakeSuccess( frame );
    }

    Common::ResultStr<GlobalDistanceField::FrameField>
    GlobalDistanceFieldView::AddFramePasses( RDG::Builder& graph, GlobalDistanceField& field,
                                             const GlobalDistanceField::ImportImage&       import,
                                             const std::vector<GlobalDistanceFieldObject>& objects,
                                             DistanceFieldSceneData& scene, const uint64_t engineFrame,
                                             const uint32_t poolMiB, const NearFieldView& view )
    {
        auto updated =
             field.AddUpdatePasses( graph, import, view.CameraPosition, objects, scene, engineFrame, poolMiB );
        if ( !updated )
            return Common::MakeError<GlobalDistanceField::FrameField>( updated.GetError() );
        GlobalDistanceField::FrameField frame = updated.GetValue();
        // This graph's view of the buffers a trace reads: the scene's records and table, the field's objects.
        using BufferImport = std::tuple<const std::shared_ptr<ShaderResources::StorageBuffer>*, RDG::ExternalBuffer*,
                                        const char*>;
        for ( const auto& [buffer, external, name] :
              { BufferImport{ &scene.GetAssetBuffer(), &m_AssetImport, "DistanceField.Assets" },
                BufferImport{ &scene.GetIndirectionBuffer(), &m_IndirectionImport, "DistanceField.Indirection" },
                BufferImport{ &field.GetObjectBuffer(), &m_ObjectImport, "DistanceField.Objects" } } )
            if ( const auto imported = Renderer::ImportBuffer( *buffer, *external ); !imported )
                return Common::MakeFormattedError<GlobalDistanceField::FrameField>( "global distance field: {}: {}",
                                                                                    name, imported.GetError() );
        frame.MeshFields.Assets      = graph.RegisterExternal( m_AssetImport, "DistanceField.Assets" );
        frame.MeshFields.Indirection = graph.RegisterExternal( m_IndirectionImport, "DistanceField.Indirection" );
        frame.MeshFields.Objects     = graph.RegisterExternal( m_ObjectImport, "DistanceField.Objects" );
        if ( const auto culled = AddNearFieldCullPasses( graph, frame, view ); !culled )
            return Common::MakeError<GlobalDistanceField::FrameField>( culled.GetError() );
        return Common::MakeSuccess( frame );
    }

    Common::BoolResultStr GlobalDistanceFieldView::AddNearFieldCullPasses( RDG::Builder&                    graph,
                                                                           GlobalDistanceField::FrameField& field,
                                                                           const NearFieldView&             view )
    {
        if ( view.Extent.x == 0 || view.Extent.y == 0 )
            return Common::MakeFormattedError<bool>( "near-field grid: the view extent {}x{} is empty",
                                                     view.Extent.x, view.Extent.y );
        MeshDistanceFieldReads& fields = field.MeshFields;
        if ( !fields.Objects.IsValid() || !fields.Assets.IsValid() )
            return Common::MakeError<bool>( "near-field grid: the mesh fields of this frame were not imported" );

        constexpr uint32_t kPixels   = static_cast<uint32_t>( DFShared::DF_MESH_SDF_GRID_PIXEL_SIZE );
        const float        kLogScale = DFShared::DF_MESH_SDF_GRID_LOG_Z_SCALE;
        const float        kLogBias  = DFShared::DF_MESH_SDF_GRID_LOG_Z_OFFSET;
        const float        kZScale   = DFShared::DF_MESH_SDF_GRID_Z_SCALE;
        const float        kMaxDepth = DFShared::DF_MESH_SDF_GRID_MAX_DISTANCE;
        const uint32_t     slices =
             static_cast<uint32_t>( std::floor( std::log2( kMaxDepth * kLogScale + kLogBias ) * kZScale ) ) + 1u;
        const glm::uvec2 cells( ( view.Extent + kPixels - 1u ) / kPixels );
        const uint32_t   total = cells.x * cells.y * slices;

        MeshSDFCullPayload cull;
        cull.Grid.WorldToClip   = view.ViewProjection;
        cull.Grid.Cells         = glm::uvec4( cells, slices, total );
        cull.Grid.ZDistribution = glm::vec4( kLogScale, kLogBias, kZScale, static_cast<float>( kPixels ) );
        cull.Grid.Screen        = glm::vec4( glm::vec2( view.Extent ), 1.0f / glm::vec2( view.Extent ) );
        cull.ClipToWorld        = view.InvViewProjection;
        cull.ClipDepth          = glm::vec4( view.Projection[2][2], view.Projection[3][2], view.Projection[2][3],
                                             view.Projection[3][3] );
        cull.Planes             = { ClipPlane( view.ViewProjection, 0, 1.0f ),
                                    ClipPlane( view.ViewProjection, 0, -1.0f ),
                                    ClipPlane( view.ViewProjection, 1, 1.0f ),
                                    ClipPlane( view.ViewProjection, 1, -1.0f ),
                                    glm::vec4( view.Forward, -glm::dot( view.Forward, view.CameraPosition ) ),
                                    glm::vec4( -view.Forward, glm::dot( view.Forward, view.CameraPosition ) + kMaxDepth ) };

        // The lists' room: what earlier frames needed (ReadNearFieldFeedback), at least the first frame's guess
        // per cell. It must fit the shaders' uint indices with the headers in front.
        const uint32_t slot  = EngineContext::GetInstance().GetCurrentFrameIndex();
        const uint32_t slots = EngineContext::GetInstance().GetMaxFramesInFlight();
        if ( m_NearFieldFeedback.size() != slots )
        {
            m_NearFieldFeedback.assign( slots, RDG::ExternalBuffer{} );
            m_NearFieldFeedbackStamp.assign( slots, 0u );
        }
        if ( slot >= slots )
            return Common::MakeFormattedError<bool>( "near-field grid: frame slot {} of {}", slot, slots );
        ReadNearFieldFeedback( slot );
        m_NearFieldListRoom = std::max<uint64_t>(
             m_NearFieldListRoom,
             uint64_t{ total } * static_cast<uint64_t>( DFShared::DF_MESH_SDF_GRID_INITIAL_OBJECTS_PER_CELL ) );
        const uint64_t gridUints = uint64_t{ total } * kGridHeaderStride + m_NearFieldListRoom;
        if ( gridUints > std::numeric_limits<uint32_t>::max() )
            return Common::MakeFormattedError<bool>(
                 "near-field grid: {} cells and {} list entries do not fit 32-bit indices", total,
                 m_NearFieldListRoom );
        m_NearFieldStamp = m_NearFieldStamp == std::numeric_limits<uint32_t>::max() ? 1u : m_NearFieldStamp + 1u;
        cull.Counts =
             glm::uvec4( fields.NumObjects, static_cast<uint32_t>( m_NearFieldListRoom ), m_NearFieldStamp, 0u );

        const RDG::BufferRef params =
             graph.CreateBuffer( RDG::BufferDesc{ sizeof( cull ) }, "DistanceField.NearField.CullParams" );
        graph.QueueBufferUpload( params, std::as_bytes( std::span( &cull, 1 ) ) );
        // The candidates' count starts at 0 (the cull appends through it).
        const std::vector<uint32_t> culledInitial( 1u + std::max( fields.NumObjects, 1u ), 0u );
        const RDG::BufferRef        culled = graph.CreateBuffer( RDG::BufferDesc{ culledInitial.size() * 4ull },
                                                                 "DistanceField.NearField.Culled" );
        graph.QueueBufferUpload( culled, std::as_bytes( std::span( culledInitial ) ) );
        const RDG::BufferRef grid =
             graph.CreateBuffer( RDG::BufferDesc{ gridUints * 4ull }, "DistanceField.NearField.Grid" );
        // The scatter's (object, key) pairs, one per list entry, sorted into the grid's lists by step 5.
        const RDG::BufferRef unsorted = graph.CreateBuffer( RDG::BufferDesc{ m_NearFieldListRoom * 8ull },
                                                            "DistanceField.NearField.Unsorted" );
        const RDG::BufferRef feedback = graph.CreateBuffer( RDG::BufferDesc{ kCullFeedbackUints * 4ull },
                                                            "DistanceField.NearField.Feedback" );

        constexpr RDG::Access kR = RDG::Access::StorageRead;
        constexpr RDG::Access kW = RDG::Access::StorageWrite;
        // ASYNC COMPUTE: the near-field cull, grid and sort nodes read the field's imports and host uploads and write
        // graph buffers only; they run with the composite on the compute queue (the graph places fork and join).
        const RDG::PassFlags kAsyncPass = RDG::PassFlags::Compute | RDG::PassFlags::AsyncCompute;
        {
            ComputePipeline* const pipeline = m_CullObjectsPipeline.get();
            const uint32_t         groups   = std::max( ( fields.NumObjects + 63u ) / 64u, 1u );
            graph.AddPass(
                 std::format( "DistanceField: Near-field cull ({} objects)", fields.NumObjects ),
                 kAsyncPass,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_CullObjectsLayout.Get( m_CullObjectsShader ),
                                    Renderer::GetPipelineRouteFill( *pipeline ) )
                          .Storage( MeshSDFCullShaderParameters::MeshSDFCullParamsUB, params, kR )
                          .Storage( MeshSDFCullShaderParameters::DFObjects, fields.Objects, kR )
                          .Storage( MeshSDFCullShaderParameters::DFAssetData, fields.Assets, kR )
                          .Storage( MeshSDFCullShaderParameters::MeshSDFCulledObjectsUB, culled, kW );
                 },
                 [pipeline, groups]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     return Renderer::DispatchCompute( bindings, *pipeline, groups, 1u, 1u );
                 } );
        }
        // Steps 2-5: count per froxel, the counts' prefix sum into the lists' first indices, scatter with the sort
        // keys, each list sorted closest first.
        const auto addGridPass = [&]( const uint32_t gridPass )
        {
            ComputePipeline* const    pipeline = m_CullGridPipeline.get();
            const uint32_t            groups   = ( total + 63u ) / 64u;
            const MeshSDFCullGridPush push{ gridPass };
            graph.AddPass(
                 std::format( "DistanceField: Near-field grid {} ({}x{}x{})", gridPass == 0u ? "count" : "scatter",
                              cells.x, cells.y, slices ),
                 kAsyncPass,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_CullGridLayout.Get( m_CullGridShader ),
                                    Renderer::GetPipelineRouteFill( *pipeline ) )
                          .Storage( MeshSDFGridShaderParameters::MeshSDFCullParamsUB, params, kR )
                          .Storage( MeshSDFGridShaderParameters::DFObjects, fields.Objects, kR )
                          .Storage( MeshSDFGridShaderParameters::MeshSDFCulledObjectsUB, culled, kR )
                          .Storage( MeshSDFGridShaderParameters::MeshSDFGridCellsUB, grid, kW )
                          .Storage( MeshSDFGridShaderParameters::MeshSDFGridUnsortedUB, unsorted, kW )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( MeshSDFCullGridPush ) ) );
                 },
                 [pipeline, groups, push]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                     return Renderer::DispatchCompute( bindings, *pipeline, groups, 1u, 1u );
                 } );
        };
        addGridPass( 0u );
        {
            ComputePipeline* const pipeline = m_CullOffsetsPipeline.get();
            graph.AddPass(
                 std::format( "DistanceField: Near-field grid offsets ({} cells)", total ),
                 kAsyncPass,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_CullOffsetsLayout.Get( m_CullOffsetsShader ),
                                    Renderer::GetPipelineRouteFill( *pipeline ) )
                          .Storage( MeshSDFGridOffsetsShaderParameters::MeshSDFCullParamsUB, params, kR )
                          .Storage( MeshSDFGridOffsetsShaderParameters::MeshSDFGridCellsUB, grid, kW )
                          .Storage( MeshSDFGridOffsetsShaderParameters::MeshSDFCullFeedbackUB, feedback, kW );
                 },
                 [pipeline]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     return Renderer::DispatchCompute( bindings, *pipeline, 1u, 1u, 1u );
                 } );
        }
        addGridPass( 1u );
        {
            // One workgroup per cell, 2D past the 65535 groups Vulkan guarantees per dimension.
            constexpr uint32_t     kMaxGroups = 65535u;
            ComputePipeline* const pipeline   = m_CullSortPipeline.get();
            const uint32_t         groupsX    = std::min( total, kMaxGroups );
            const uint32_t         groupsY    = ( total + groupsX - 1u ) / groupsX;
            graph.AddPass(
                 std::format( "DistanceField: Near-field grid sort ({} cells)", total ), kAsyncPass,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_CullSortLayout.Get( m_CullSortShader ),
                                    Renderer::GetPipelineRouteFill( *pipeline ) )
                          .Storage( MeshSDFGridSortShaderParameters::MeshSDFCullParamsUB, params, kR )
                          .Storage( MeshSDFGridSortShaderParameters::MeshSDFGridCellsUB, grid, kW )
                          .Storage( MeshSDFGridSortShaderParameters::MeshSDFGridUnsortedUB, unsorted, kR );
                 },
                 [pipeline, groupsX, groupsY]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     return Renderer::DispatchCompute( bindings, *pipeline, groupsX, groupsY, 1u );
                 } );
        }
        graph.Extract( feedback, m_NearFieldFeedback[slot], RDG::Access::HostRead );
        m_NearFieldFeedbackStamp[slot] = m_NearFieldStamp;
        fields.NearFieldGrid           = grid;
        fields.NearFieldGridPlacement  = cull.Grid;
        return BOOLSUCCESS;
    }

    void GlobalDistanceFieldView::ReadNearFieldFeedback( const uint32_t slot )
    {
        RDG::ExternalBuffer& readback = m_NearFieldFeedback[slot];
        const uint32_t       stamp    = std::exchange( m_NearFieldFeedbackStamp[slot], 0u );
        if ( stamp == 0u || !readback.Physical )
            return; // no cull ran in this slot yet, or its graph faulted before the extract
        const std::span<const std::byte>         bytes = readback.Physical->ReadForHost();
        std::array<uint32_t, kCullFeedbackUints> values{};
        if ( bytes.size() < sizeof( values ) )
        {
            LOG_ERROR( "[DistanceField] the near-field cull's feedback is not host-readable ({} bytes)",
                       bytes.size() );
            return;
        }
        std::memcpy( values.data(), bytes.data(), sizeof( values ) );
        readback.Physical.reset();
        const auto [needed, room, longest, written] = values;
        if ( written != stamp )
            return; // another cull's bytes: nothing to learn from them
        if ( !m_NearFieldFeedbackLogged )
        {
            m_NearFieldFeedbackLogged = true;
            LOG_INFO( "[DistanceField] near-field object lists: {} entries, the longest {}, room for {}", needed,
                      longest, room );
        }
        if ( needed <= room )
            return;
        m_NearFieldListRoom = std::max( m_NearFieldListRoom, uint64_t{ needed } + uint64_t{ needed } / 4u );
        // The other slots culled with the same room report the same refusal: one line per room.
        if ( std::exchange( m_NearFieldRefusedRoom, room ) == room )
            return;
        LOG_WARN(
             "[DistanceField] near-field object lists needed {} entries (longest {}), the grid had room for {}: "
             "that frame's near field was refused (global field only); room for {} from now on",
             needed, longest, room, m_NearFieldListRoom );
    }

    Common::BoolResultStr GlobalDistanceFieldView::PrepareDebugPipeline( const Core::Formats::ImageFormat format,
                                                                     const uint32_t                   samples )
    {
        const auto key = std::make_pair( static_cast<int>( format ), samples );
        if ( const auto it = m_ViewPipelines.find( key ); it != m_ViewPipelines.end() )
        {
            m_ViewPipeline = it->second;
            return BOOLSUCCESS;
        }
        GraphicsPipelineSpecification spec;
        spec.DebugName = "GlobalDistanceFieldView";
        spec.TargetLayout =
             RenderTargetLayout{ .ColorFormats = { format }, .DepthFormat = std::nullopt, .Samples = samples };
        spec.Shader            = m_ViewShader;
        spec.DepthTestEnabled  = false;
        spec.DepthWriteEnabled = false;
        const auto pipeline    = GraphicsPipeline::Create( spec );
        if ( !pipeline )
            return Common::MakeError<bool>( pipeline.GetError() );
        m_ViewPipeline = m_ViewPipelines.emplace( key, pipeline.GetValue() ).first->second;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr GlobalDistanceFieldView::AddDebugViewPass( RDG::Builder&                          graph,
                                                                     const GlobalDistanceField::FrameField& field,
                                                                     const RDG::TextureRef                  target,
                                                                     const RDG::LoadOp&                     load,
                                                                     const glm::mat4& inverseViewProjection )
    {
        const auto desc = graph.GetTextureDesc( target );
        if ( !desc )
            return Common::MakeError<bool>( desc.GetError() );
        if ( const auto prepared = PrepareDebugPipeline( desc.GetValue().Format, desc.GetValue().Samples );
             !prepared )
            return prepared;
        GdfViewPush push;
        push.InverseViewProjection = inverseViewProjection;
        for ( int level = 0; level < kGlobalDistanceFieldClipmaps; ++level )
            push.Levels[level] = field.Levels[level];
        GraphicsPipeline* pipeline = m_ViewPipeline.get();
        graph.AddPass(
             "Debug: Global Distance Field", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 auto block = pass.Bindings( m_ViewLayout.Get( m_ViewShader ), RDG::OtherRouteFill{} );
                 for ( int level = 0; level < kGlobalDistanceFieldClipmaps; ++level )
                     block.Texture( kGlobalDistanceFieldClipmapTextures[level], field.Clipmaps[level],
                                    RDG::Access::SampledGraphics, RDG::SubresourceRange::All() )
                          .Sampler( GlobalDistanceFieldViewShaderParameters::LinearRepeatSampler,
                                    RDG::SamplerDesc::LinearRepeat() );
                 block.PushConstantBytes( static_cast<uint32_t>( sizeof( GdfViewPush ) ) );
                 pass.ColorTarget( 0, target, load );
             },
             [pipeline, push]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                 return Renderer::GetInstance().DrawFullscreen( bindings, *pipeline, nullptr );
             } );
        return BOOLSUCCESS;
    }
} // namespace Desert::Graphic::DistanceField
