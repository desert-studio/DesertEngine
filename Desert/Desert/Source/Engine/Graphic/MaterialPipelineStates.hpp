#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

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

    private:
        mutable std::mutex       m_Mutex;
        std::vector<std::string> m_Names;
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

        [[nodiscard]] size_t Count( const MaterialPipelineState state ) const
        {
            size_t n = 0;
            for ( const auto& [name, row] : m_Rows )
                n += row.State == state ? 1 : 0;
            return n;
        }

    private:
        struct Row
        {
            MaterialPipelineState State     = MaterialPipelineState::Requested;
            uint8_t               Announced = 0; ///< one bit per state already logged
        };
        std::unordered_map<std::string, Row> m_Rows;
    };
} // namespace Desert::Graphic
