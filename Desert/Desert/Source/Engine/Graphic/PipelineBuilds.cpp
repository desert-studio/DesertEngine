#include <Engine/Graphic/PipelineBuilds.hpp>

namespace Desert::Graphic::PipelineBuilds
{
    Tracker& Get()
    {
        static Tracker tracker;
        return tracker;
    }
}
