#include <Engine/ShaderResources/ViewCopiedBlock.hpp>

namespace Desert::ShaderResources
{
    auto DescriptorCopyRecord::RecordIdentity::Next() noexcept -> uint64_t
    {
        static std::atomic<uint64_t> s_Next{ 1 };
        return s_Next.fetch_add( 1, std::memory_order_relaxed );
    }

    auto ViewCopiedBlock::StampCopyEpoch() noexcept -> void
    {
        static std::atomic<uint64_t> s_Next{ 0 };
        m_CopyEpoch.store( s_Next.fetch_add( 1, std::memory_order_relaxed ) + 1, std::memory_order_release );
    }
}
