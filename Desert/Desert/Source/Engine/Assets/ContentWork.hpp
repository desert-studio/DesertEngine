#pragma once

#include <Engine/Assets/AsyncAssetLoader.hpp>
#include <Engine/Graphic/PipelineBuilds.hpp>

#include <cstddef>
#include <cstdint>

namespace Desert::Assets
{
    // What ContentGate waits for, gathered in ONE place for both hosts: asset reads on workers AND the ENGINE's
    // graphics pipelines still in the driver (PSO1). Material pipelines are deliberately absent (AL1-12): their
    // draws fall back to the engine's default surface while they compile, so waiting for them would only hold
    // the window closed for seconds on a cold pipeline cache. A world whose meshes have landed but whose system
    // pipelines have not would open the gate on a frame that skips them — the same missing-object defect the gate exists for,
    // moved from the file system to the driver. The counters are summed: "outstanding" is work not yet done,
    // "started" is monotonic, so ContentGate's two conditions keep their meaning over the sum.
    struct ContentWork
    {
        size_t   Outstanding = 0;
        uint64_t Started     = 0;
    };

    inline ContentWork ContentWorkNow()
    {
        const auto& loader    = AsyncAssetLoader::Get();
        const auto& pipelines = Graphic::PipelineBuilds::Get();
        return { loader.Outstanding() + pipelines.Pending( Graphic::PipelineRole::Engine ),
                 loader.StartedCount() + pipelines.Started( Graphic::PipelineRole::Engine ) };
    }
} // namespace Desert::Assets
