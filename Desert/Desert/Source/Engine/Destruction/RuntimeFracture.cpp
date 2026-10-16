#include <Engine/Destruction/RuntimeFracture.hpp>

#include <Common/Core/JobSystem.hpp>

#include <algorithm>
#include <chrono>
#include <exception>
#include <format>
#include <utility>

namespace Desert::Destruction
{
    uint64_t MixSeed( uint64_t a, uint64_t b )
    {
        uint64_t x = a ^ ( b + 0x9E3779B97F4A7C15ull + ( a << 6 ) + ( a >> 2 ) );
        x          = ( x ^ ( x >> 30 ) ) * 0xBF58476D1CE4E5B9ull;
        x          = ( x ^ ( x >> 27 ) ) * 0x94D049BB133111EBull;
        return x ^ ( x >> 31 );
    }

    Common::ResultStr<RuntimeFractureResult> BakeRuntimeFracture( const Geometry::DynamicMesh3& source,
                                                                  const RuntimeFractureRequest& request )
    {
        const auto began = std::chrono::steady_clock::now();
        auto       baked = BakeFracture( source, request.Settings );
        if ( !baked )
            return Common::MakeError<RuntimeFractureResult>( baked.GetError() );
        FractureBakeResult result = baked.ExtractValue();

        auto data                = std::make_shared<FractureData>();
        data->SourceMesh         = request.SourceMesh;
        data->Settings           = request.Settings;
        data->InteriorMaterialId = result.InteriorMaterialId;
        data->InteriorMaterial   = request.InteriorMaterial;
        data->Nodes              = std::move( result.Nodes );

        RuntimeFractureResult out;
        out.Data   = std::move( data );
        out.BakeMs = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - began ).count();
        return Common::MakeSuccess( std::move( out ) );
    }

    RuntimeFractureQueue::RuntimeFractureQueue() : m_Sources( std::make_shared<SourceCache>() )
    {
    }

    RuntimeFractureQueue::~RuntimeFractureQueue() = default;

    uint32_t RuntimeFractureQueue::MaxConcurrent()
    {
        return std::max<uint32_t>( 1u, static_cast<uint32_t>( Common::JobSystem::Get().WorkerCount() / 2 ) );
    }

    RuntimeFractureQueue::Ticket RuntimeFractureQueue::Submit( RuntimeFractureRequest request )
    {
        const Ticket ticket = m_Next++;
        m_Queued.emplace_back( ticket, std::move( request ) );
        Start();
        return ticket;
    }

    void RuntimeFractureQueue::Start()
    {
        std::erase_if( m_Cancelled, []( const auto& future )
                       { return future.wait_for( std::chrono::seconds( 0 ) ) == std::future_status::ready; } );
        while ( !m_Queued.empty() && Running() < MaxConcurrent() )
        {
            auto [ticket, request] = std::move( m_Queued.front() );
            m_Queued.pop_front();
            m_Running.emplace( ticket,
                               Common::JobSystem::Get().Async(
                                    [sources = m_Sources,
                                     request = std::move( request )]() -> Common::ResultStr<RuntimeFractureResult>
                                    {
                                        // One lift per source: the first cut of a mesh reads it, the others share
                                        // it.
                                        std::shared_ptr<const Geometry::DynamicMesh3> mesh;
                                        {
                                            std::lock_guard lock( sources->Mutex );
                                            if ( const auto found = sources->Meshes.find( request.SourceKey );
                                                 found != sources->Meshes.end() )
                                                mesh = found->second;
                                        }
                                        if ( !mesh )
                                        {
                                            auto made = request.MakeSource();
                                            if ( !made )
                                                return Common::MakeFormattedError<RuntimeFractureResult>(
                                                     "its source mesh ({}) cannot be read: {}", request.SourceKey,
                                                     made.GetError() );
                                            mesh = made.ExtractValue();
                                            std::lock_guard lock( sources->Mutex );
                                            sources->Meshes.emplace( request.SourceKey, mesh );
                                        }
                                        return BakeRuntimeFracture( *mesh, request );
                                    } ) );
        }
    }

    Common::ResultStr<RuntimeFractureResult> RuntimeFractureQueue::Poll( Ticket ticket )
    {
        Start();
        const auto running = m_Running.find( ticket );
        if ( running == m_Running.end() )
        {
            const bool queued = std::any_of( m_Queued.begin(), m_Queued.end(),
                                             [ticket]( const auto& entry ) { return entry.first == ticket; } );
            if ( queued )
                return Common::MakeSuccess( RuntimeFractureResult{} );
            return Common::MakeFormattedError<RuntimeFractureResult>( "run-time fracture {} is unknown", ticket );
        }
        if ( running->second.wait_for( std::chrono::seconds( 0 ) ) != std::future_status::ready )
            return Common::MakeSuccess( RuntimeFractureResult{} );

        auto future = std::move( running->second );
        m_Running.erase( running );
        Start(); // the freed slot goes to the next queued cut now, not next frame
        try
        {
            return future.get();
        }
        catch ( const std::exception& error )
        {
            // The pool dropped the job unrun (shutting down) or the bake threw.
            return Common::MakeFormattedError<RuntimeFractureResult>( "the fracture job did not run: {}",
                                                                      error.what() );
        }
    }

    void RuntimeFractureQueue::Cancel( Ticket ticket )
    {
        std::erase_if( m_Queued, [ticket]( const auto& entry ) { return entry.first == ticket; } );
        // A packaged task's future does not block: the job finishes into nothing, counted until it has.
        if ( const auto running = m_Running.find( ticket ); running != m_Running.end() )
        {
            m_Cancelled.push_back( std::move( running->second ) );
            m_Running.erase( running );
        }
    }
} // namespace Desert::Destruction
