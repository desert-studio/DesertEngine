#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <Engine/Graphic/Materials/Mesh/MeshVertexPath.hpp>

// Material pipelines ON DEMAND (AL1-12), the pattern of UE 5.8's PSO precache (PipelineStateCache.cpp,
// FMaterialPSOPrecacheRequest): global shaders and the engine's system passes are built at startup and the
// reveal waits for them; a content material's pipeline is REQUESTED when the material loads, compiled on a
// worker through the disk-persisted VkPipelineCache, and until it is ready the material's draws use the
// engine's default surface — never a stall, never skipped silently.
//
// Pure std, no device: the state machine and the draw's choice are what `Desert/Tests/Engine/PipelineCacheFile`
// pins; MeshRenderer feeds it the readiness its pipelines report.
namespace Desert::Graphic
{
    // Where one material's pipeline stands in ONE renderer. The key is the material's shader name, which is
    // what its pipeline is built from (the pipeline cache key adds only renderer-owned fields to it).
    enum class MaterialPipelineState : uint8_t
    {
        Requested, ///< asked for (on load); no compile handed to a worker yet
        Compiling, ///< on a worker; draws use the default surface
        Ready,     ///< draws use the material's own pipeline
        Failed,    ///< the driver or the spec refused it; draws use the default surface, the reason is logged
    };

    [[nodiscard]] inline const char* MaterialPipelineStateName( const MaterialPipelineState state )
    {
        switch ( state )
        {
            case MaterialPipelineState::Requested:
                return "requested";
            case MaterialPipelineState::Compiling:
                return "compiling";
            case MaterialPipelineState::Ready:
                return "ready";
            case MaterialPipelineState::Failed:
                return "failed";
        }
        return "unknown";
    }

    // The process-wide list of materials whose pipelines were asked for when they LOADED (MaterialService).
    // Append-only: every renderer reads it from its own cursor, so a material loaded before a preview
    // viewport existed is still precached by that viewport, and nobody has to know how many renderers live.
    class MaterialPipelineRequests
    {
    public:
        static MaterialPipelineRequests& Get()
        {
            static MaterialPipelineRequests requests;
            return requests;
        }

        // Loads may land on a worker, hence the lock. A name asked for twice is listed once.
        void Request( const std::string& shaderName )
        {
            const std::lock_guard lock( m_Mutex );
            for ( const auto& name : m_Names )
                if ( name == shaderName )
                    return;
            m_Names.push_back( shaderName );
        }

        // The names requested since @p cursor, which is advanced past them.
        std::vector<std::string> Since( size_t& cursor ) const
        {
            const std::lock_guard    lock( m_Mutex );
            std::vector<std::string> fresh;
            for ( ; cursor < m_Names.size(); ++cursor )
                fresh.push_back( m_Names[cursor] );
            return fresh;
        }

        // A MESH-TABLE CELL's pipeline (GRASS-SETTLE): a cell is drawn on the batched path, whose pipeline is the
        // pass state x the cell x the cull permutation (MeshRenderer::CellPipeline / CullPermutation), so it is
        // precached per (vertex path x pass) with the material's two-sidedness, as UE's FMaterialPSOPrecacheParams
        // carry the vertex factory and the pass. Without it a cell's pipeline was first asked for by the frame
        // that saw its mesh, which then drew the default surface for seconds (FIELD_Barn: a pale grey-blue
        // patch of foliage with a straight edge on the first frame after the camera moved).
        struct CellRequest
        {
            std::string    Shader; ///< the cell program, "<Template>/<Path>.<Pass>" (MaterialService CellShaderOf)
            MeshVertexPath Path     = MeshVertexPath::Static;
            MeshPass       Pass     = MeshPass::GBuffer;
            bool           TwoSided = false;

            bool operator==( const CellRequest& ) const = default;
        };

        void RequestCell( CellRequest request )
        {
            const std::lock_guard lock( m_Mutex );
            for ( const auto& cell : m_Cells )
                if ( cell == request )
                    return;
            m_Cells.push_back( std::move( request ) );
        }

        std::vector<CellRequest> CellsSince( size_t& cursor ) const
        {
            const std::lock_guard    lock( m_Mutex );
            std::vector<CellRequest> fresh;
            for ( ; cursor < m_Cells.size(); ++cursor )
                fresh.push_back( m_Cells[cursor] );
            return fresh;
        }

    private:
        mutable std::mutex       m_Mutex;
        std::vector<std::string> m_Names;
        std::vector<CellRequest> m_Cells;
    };

    // One renderer's view: which material pipelines it has asked for and what they are doing, and the draw's
    // choice between the material's own pipeline and the default surface.
    class MaterialPipelineTracker
    {
    public:
        // Requested, unless it is already known (a request never moves a material backwards).
        void Request( const std::string& shaderName )
        {
            m_Rows.try_emplace( shaderName, Row{} );
        }

        // What the renderer's pipeline for @p shaderName reports this frame. A Ready material that reports
        // Compiling again (shader hot-reload dropped and rebuilt its pipeline) goes back to the default surface.
        void OnCompiling( const std::string& shaderName )
        {
            m_Rows[shaderName].State = MaterialPipelineState::Compiling;
        }
        void OnReady( const std::string& shaderName )
        {
            m_Rows[shaderName].State = MaterialPipelineState::Ready;
        }
        void OnFailed( const std::string& shaderName )
        {
            m_Rows[shaderName].State = MaterialPipelineState::Failed;
        }

        [[nodiscard]] std::optional<MaterialPipelineState> StateOf( const std::string& shaderName ) const
        {
            const auto it = m_Rows.find( shaderName );
            if ( it == m_Rows.end() )
                return std::nullopt;
            return it->second.State;
        }

        struct DrawChoice
        {
            bool UseOwnPipeline = false;
            // True the FIRST time this material is drawn with the default surface in this state: the caller
            // logs then, once per material per state, not sixty times a second.
            bool Announce = false;
        };

        // Own pipeline only when Ready; every other state draws the default surface. An unknown material
        // is a draw the renderer never asked for — it is Requested here, so it is not silently lost either.
        DrawChoice Choose( const std::string& shaderName )
        {
            auto& row = m_Rows[shaderName];
            if ( row.State == MaterialPipelineState::Ready )
                return { true, false };
            const auto bit      = static_cast<uint8_t>( 1u << static_cast<uint8_t>( row.State ) );
            const bool announce = ( row.Announced & bit ) == 0;
            row.Announced       = static_cast<uint8_t>( row.Announced | bit );
            return { false, announce };
        }

        // What a GRAPH PASS records for one content cell (AL1-12 inside RDG). A pass records only pipelines it can
        // bind: binding one still in the driver fails the pass, and a failed pass drops the WHOLE frame graph —
        // every view of the frame, for one material. So the cell's own pipeline only when it is Ready; otherwise
        // the pass's default surface, and nothing only while that stand-in is not Ready either (before the reveal,
        // which waits for it). @p key names the cell's PIPELINE (pass state x cell x cull permutation): one shader
        // has a pipeline per pass, and those are ready at different moments.
        enum class CellDraw : uint8_t
        {
            Own,            ///< the material's own pipeline, Ready
            DefaultSurface, ///< the pass's default surface stands in
            Nothing,        ///< neither is Ready yet: the object is not recorded this frame
        };
        struct CellChoice
        {
            CellDraw Draw     = CellDraw::Nothing;
            bool     Announce = false; ///< first time for this cell in this state: the caller logs once
        };
        // A graph pass's cell by IDENTITY, never by a name built per draw (CPU-FRAME-ARENA 6; UE keys its PSO
        // cache by the initializer's state, not a string): the cell's own pipeline when it has one (one per pass
        // state x cell x cull permutation, rebuilt in place by a recompile), else the pass state x the cell
        // material x two-sidedness that would have built it. Its row carries the state reported this frame and
        // the announce bits; the state is re-reported by every call, so an address a freed object hands to a
        // new one can only skip one announce line, never change a draw.
        struct CellKey
        {
            const void* Pipeline  = nullptr; ///< the cell's own pipeline, or null when it was refused
            const void* PassState = nullptr; ///< refused only: the pass state the cell is drawn through
            const void* Cell      = nullptr; ///< refused only: the cell material
            bool        TwoSided  = false;   ///< refused only: the cull permutation it asked for

            bool operator==( const CellKey& ) const = default;
        };
        struct CellKeyHash
        {
            size_t operator()( const CellKey& key ) const noexcept
            {
                const auto mix = []( const size_t seed, const size_t value ) noexcept
                { return seed ^ ( value + 0x9e3779b97f4a7c15ull + ( seed << 6 ) + ( seed >> 2 ) ); };
                size_t seed = std::hash<const void*>{}( key.Pipeline );
                seed        = mix( seed, std::hash<const void*>{}( key.PassState ) );
                seed        = mix( seed, std::hash<const void*>{}( key.Cell ) );
                return mix( seed, static_cast<size_t>( key.TwoSided ) );
            }
        };

        CellChoice ChooseCell( const std::string& key, const MaterialPipelineState ownReports,
                               const bool defaultSurfaceReady )
        {
            return ChooseRow( m_Rows[key], ownReports, defaultSurfaceReady );
        }
        CellChoice ChooseCell( const CellKey& key, const MaterialPipelineState ownReports,
                               const bool defaultSurfaceReady )
        {
            return ChooseRow( m_CellRows[key], ownReports, defaultSurfaceReady );
        }
        [[nodiscard]] std::optional<MaterialPipelineState> StateOf( const CellKey& key ) const
        {
            const auto it = m_CellRows.find( key );
            if ( it == m_CellRows.end() )
                return std::nullopt;
            return it->second.State;
        }

        [[nodiscard]] size_t Count( const MaterialPipelineState state ) const
        {
            size_t n = 0;
            for ( const auto& [name, row] : m_Rows )
                n += row.State == state ? 1 : 0;
            for ( const auto& [key, row] : m_CellRows )
                n += row.State == state ? 1 : 0;
            return n;
        }

    private:
        struct Row
        {
            MaterialPipelineState State     = MaterialPipelineState::Requested;
            uint8_t               Announced = 0; ///< one bit per state already logged
        };
        // The state @p ownReports moves @p row to (a Requested report never moves it backwards), then the draw.
        static CellChoice ChooseRow( Row& row, const MaterialPipelineState ownReports,
                                     const bool defaultSurfaceReady ) noexcept
        {
            if ( ownReports != MaterialPipelineState::Requested )
                row.State = ownReports;
            if ( row.State == MaterialPipelineState::Ready )
                return { CellDraw::Own, false };
            const auto bit      = static_cast<uint8_t>( 1u << static_cast<uint8_t>( row.State ) );
            const bool announce = ( row.Announced & bit ) == 0;
            row.Announced       = static_cast<uint8_t>( row.Announced | bit );
            return { defaultSurfaceReady ? CellDraw::DefaultSurface : CellDraw::Nothing, announce };
        }

        std::unordered_map<std::string, Row>          m_Rows;
        std::unordered_map<CellKey, Row, CellKeyHash> m_CellRows; ///< graph-pass cells (ChooseCell by CellKey)
    };
} // namespace Desert::Graphic
