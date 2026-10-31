#pragma once

#include <Engine/Core/FrameManager.hpp>

#include <cstdint>
#include <mutex>
#include <optional>

// WHAT THE RAY TRACING PIPELINES WROTE INTO THEIR PERSISTENT SHADER BINDING TABLES, per frame (UE: the dirty
// records an FRayTracingShaderBindingTable update uploads). The backend adds one trace's counts as it writes them
// (VulkanRayTracingPipeline::RecordTraceRays); the viewport's ray tracing overlay reads the last finished frame's
// sums. On a still scene Records and Rows are 0 once every frame slot's table has been written. Data + one
// process-wide accumulator: the tables belong to pipelines that several views share, so the sum is the device's,
// not a view's.
namespace Desert::Graphic::RayTracing
{
    struct RayTracingSbtWriteCounts
    {
        uint32_t Traces       = 0;
        uint32_t Records      = 0; // table records written (ray generation, miss and hit records)
        uint32_t RecordsTotal = 0; // table records the traces read
        uint32_t Rows         = 0; // 16-byte material rows written
        uint32_t RowsTotal    = 0; // material rows the traces read

        RayTracingSbtWriteCounts& operator+=( const RayTracingSbtWriteCounts& other )
        {
            Traces += other.Traces;
            Records += other.Records;
            RecordsTotal += other.RecordsTotal;
            Rows += other.Rows;
            RowsTotal += other.RowsTotal;
            return *this;
        }
        bool operator==( const RayTracingSbtWriteCounts& ) const = default;
    };

    class RayTracingSbtWrites
    {
    public:
        // One trace's counts, into the frame being recorded. When this is the first trace of a new frame and the
        // frame before it wrote a different amount than the last frame reported here, answers that frame's sums
        // (the backend logs them: a still scene logs its last nonzero frame, then one zero frame, then nothing).
        static std::optional<RayTracingSbtWriteCounts> Add( RayTracingSbtWriteCounts counts )
        {
            counts.Traces               = 1;
            State&                state = Get();
            const uint64_t        frame = ::Desert::Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();
            const std::lock_guard lock( state.Mutex );
            std::optional<RayTracingSbtWriteCounts> finished;
            if ( frame != state.Frame )
            {
                if ( state.Frame != kNoFrame && ( !state.Reported || *state.Reported != state.Current ) )
                {
                    finished       = state.Current;
                    state.Reported = state.Current;
                }
                state.Previous      = state.Current;
                state.PreviousFrame = state.Frame;
                state.Current       = {};
                state.Frame         = frame;
            }
            state.Current += counts;
            return finished;
        }

        // The sums of the frame before the one being recorded; all zero when no pipeline traced in it.
        static RayTracingSbtWriteCounts LastFrame()
        {
            State&                state = Get();
            const uint64_t        frame = ::Desert::Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();
            const std::lock_guard lock( state.Mutex );
            if ( frame == 0 )
                return {};
            if ( state.Frame == frame - 1u )
                return state.Current;
            if ( state.Frame == frame && state.PreviousFrame == frame - 1u )
                return state.Previous;
            return {};
        }

    private:
        static constexpr uint64_t kNoFrame = UINT64_MAX;
        struct State
        {
            std::mutex                              Mutex;
            uint64_t                                Frame         = kNoFrame;
            uint64_t                                PreviousFrame = kNoFrame;
            RayTracingSbtWriteCounts                Current;
            RayTracingSbtWriteCounts                Previous;
            std::optional<RayTracingSbtWriteCounts> Reported;
        };
        static State& Get()
        {
            static State state;
            return state;
        }
    };
} // namespace Desert::Graphic::RayTracing
