#include <Engine/Graphic/RDG/RDGRecordPrerequisite.hpp>

namespace Desert::Graphic::RDG
{
    auto RecordingSeal::IsSealed() noexcept -> bool
    {
        return s_Depth != 0;
    }

    RecordingSeal::~RecordingSeal() noexcept
    {
        --s_Depth;
    }

    RecordingSeal::RecordingSeal() noexcept
    {
        ++s_Depth;
    }

    thread_local uint32_t RecordingSeal::s_Depth = 0;
}
