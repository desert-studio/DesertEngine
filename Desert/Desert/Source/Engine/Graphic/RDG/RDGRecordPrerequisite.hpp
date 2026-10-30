#pragma once

#include <Common/Core/ResultStr.hpp>

#include <cstdint>

namespace Desert::Graphic::RDG
{
    /**
     * @brief Something a pass's exec uses that has to EXIST before any pass of the graph records (RENDER-THREADS
     * stage 1, migration M1).
     *
     * UE RDG parallel execute: a pass lambda only binds what its setup prepared - the pass parameters, uniform
     * buffers and descriptor sets are created before the graph records, so recording allocates nothing and
     * touches no shared mutable state. A pass declares each such object in its setup (PassBuilder::Prepares);
     * Builder::Execute calls PrepareForRecording once per object per graph, sequentially, on the calling thread,
     * after Compile and before the first pass records. Only passes that survive Compile are prepared.
     *
     * The declaration is also the pass's promise that its exec creates nothing: the exec of a pass with at least
     * one prerequisite runs inside a RecordingSeal, and the per-view resources refuse to be created under it.
     */
    class IRecordPrerequisite
    {
    public:
        virtual ~IRecordPrerequisite() = default;

        // Creates what the exec will look up. Sequential; an error faults the declaring pass (Execution stage)
        // and its exec does not run. Const: what it makes and writes is the object's per-view cache (a material's
        // view sets and block copies), not its value - so a pass declares the const material its setup holds.
        [[nodiscard]] virtual Common::BoolResultStr PrepareForRecording() const = 0;
    };

    /**
     * @brief Marks the calling thread as recording the exec of a pass that declared its prerequisites.
     *
     * Per thread on purpose: a loader thread writing a buffer outside every view while the render thread records
     * is not recording, and once chunks record in parallel each worker opens its own seal around the passes it
     * records. RAII, nestable.
     */
    class RecordingSeal
    {
    public:
        RecordingSeal() noexcept
        {
            ++s_Depth;
        }
        ~RecordingSeal() noexcept
        {
            --s_Depth;
        }

        RecordingSeal( const RecordingSeal& )            = delete;
        RecordingSeal& operator=( const RecordingSeal& ) = delete;
        RecordingSeal( RecordingSeal&& )                 = delete;
        RecordingSeal& operator=( RecordingSeal&& )      = delete;

        // True while this thread records a sealed exec: a per-view resource that does not exist yet is an
        // undeclared prerequisite and is refused, never created.
        [[nodiscard]] static bool IsSealed() noexcept
        {
            return s_Depth != 0;
        }

    private:
        static inline thread_local uint32_t s_Depth = 0;
    };
} // namespace Desert::Graphic::RDG
