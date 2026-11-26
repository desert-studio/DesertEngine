#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace Desert::Engine
{
    struct DeviceMemoryReport;
}

namespace Desert::Graphic
{
    /**
     * THE VIDEO MEMORY THE ELASTIC CONSUMERS SHARE (RT-WIN-PROOF-MEM).
     *
     * UE sizes its streaming pool from the card and caps the shadow denoiser by a fixed light count
     * (r.Shadow.Denoiser.MaxLights): two constants that do not know of each other nor of what else the machine
     * runs. Here the pool is measured - the implementation's live budget (VK_EXT_memory_budget), less a reserve,
     * less everything this process holds outside the clients - so it is right on any card and shrinks when another
     * application takes memory.
     *
     * A client declares what it cannot work below, what it wants and what it holds, and takes its grant. The grant
     * is a pure function of the accepted pool and of every declaration: minimums first, the rest shared by weight
     * up to what each wants - and no byte is granted to one client while another still holds it: growth is shared
     * out of what nobody holds. The HYSTERESIS is on the pool alone, so a moving camera redistributes at once
     * while a breathing driver number moves nothing. The pool SHRINKS at once and GROWS by a part of the gap per
     * settle period, so what the rest of the process allocates meanwhile is measured before it is handed out.
     *
     * Thread-safe; Update is the frame loop's.
     */
    class GpuMemoryBudget
    {
    public:
        static GpuMemoryBudget& Get();

        /// One consumer. Registered for its lifetime; what it declared leaves the budget with it.
        class Client
        {
        public:
            /// @p name is what the log calls it; @p weight its priority - its share of what the minimums leave.
            Client( std::string name, float weight );
            ~Client();
            Client( const Client& )            = delete;
            Client& operator=( const Client& ) = delete;
            Client( Client&& )                 = delete;
            Client& operator=( Client&& )      = delete;

            /// Bytes: @p minimum it cannot work below, @p desired all it could use, @p held what it holds on the
            /// device now, memory it released that frames in flight still keep included - the part of the process'
            /// usage the pool may count on. Re-declare whenever any of them moved.
            void Declare( uint64_t minimum, uint64_t desired, uint64_t held );
            /// The bytes it may hold. Before the first measurement (no device: the tools and the suites) there is
            /// nothing to share out and it is `desired`.
            [[nodiscard]] uint64_t Grant() const;
            /// Another client is granted less than it wants: what this one holds and does not use is owed to it.
            [[nodiscard]] bool OthersWait() const;

        private:
            uint64_t m_Id = 0;
        };

        /// The frame's reading. Called once a frame, in every configuration.
        void Update( const Engine::DeviceMemoryReport& report );

        /// THE TEXTURE STREAMING POOL'S ROW IN THE MEMORY CENSUS (TEX-STREAMING). Written by the texture service
        /// each frame, read by the allocator's census (any thread), so the census can say what the pool holds
        /// against its budget. Kept here, beside the budget the census reports, and not with the streaming
        /// demand: the allocator that prints it is RenderCore's (BINT-RC).
        struct StreamingPoolStats
        {
            std::atomic<uint64_t> ResidentBytes{ 0 };
            std::atomic<uint64_t> WantedBytes{ 0 };
            std::atomic<uint64_t> BudgetBytes{ 0 };
            std::atomic<uint64_t> FullChainBytes{ 0 };
            std::atomic<uint32_t> Textures{ 0 };
            std::atomic<uint32_t> Streaming{ 0 };
        };
        [[nodiscard]] StreamingPoolStats& TextureStreamingPool();

    private:
        struct Row
        {
            uint64_t    Id = 0;
            std::string Name;
            float       Weight        = 1.0f;
            uint64_t    Minimum       = 0;
            uint64_t    Desired       = 0;
            uint64_t    Held          = 0;
            uint64_t    Target        = 0; // its share of the pool
            uint64_t    Grant         = 0; // the part of the target nobody else still holds
            bool        LoggedLimited = false;
        };

        GpuMemoryBudget() = default;

        uint64_t           Register( std::string name, float weight );
        void               Unregister( uint64_t id );
        [[nodiscard]] Row* Find( uint64_t id );
        // The grants from the accepted pool and the declarations.
        void Distribute();
        // Raises each row's @p given towards its @p wanted out of @p left, by weight.
        void Share( uint64_t Row::* given, uint64_t Row::* wanted, uint64_t left );
        // Takes @p pool as the accepted one, and says so.
        void Accept( uint64_t pool, uint64_t budget, uint64_t usage );
        void LogClients();

        std::mutex       m_Lock; // everything below
        std::vector<Row> m_Rows;
        uint64_t         m_NextId         = 1;
        uint64_t         m_Pool           = 0;
        bool             m_Measured       = false;
        uint32_t         m_ShrinkFrames   = 0; // consecutive frames the measurement asked for less / more
        uint32_t         m_GrowFrames     = 0;
        uint32_t         m_SettleFrames   = 0; // frames left before the next shrink may be accepted
        uint32_t         m_LogWaitFrames  = 0; // frames left before a limited/unlimited flip may be logged
        bool             m_WarnedMinimums = false;
        bool             m_WarnedUnknown  = false;

        StreamingPoolStats m_TextureStreamingPool; // atomics, outside m_Lock
    };
} // namespace Desert::Graphic
