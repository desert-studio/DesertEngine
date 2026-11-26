#include <Engine/Graphic/GpuDriven/IndirectSubmissionMode.hpp>

#include <algorithm>

namespace Desert::Graphic::GpuDriven
{
    IndirectSubmissionMode SelectIndirectSubmission( const IndirectDrawCapabilities& capabilities )
    {
        if ( capabilities.MultiDrawIndirect )
            return { .Submission      = IndirectSubmission::DrawIndexedIndirectMulti,
                     .MaxDrawsPerCall = std::max( capabilities.MaxDrawIndirectCount, 1u ) };
        return { .Submission = IndirectSubmission::DrawIndexedIndirectPerRecord, .MaxDrawsPerCall = 1u };
    }
} // namespace Desert::Graphic::GpuDriven
