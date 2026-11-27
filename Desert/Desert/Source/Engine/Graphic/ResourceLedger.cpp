#include <Engine/Graphic/ResourceLedger.hpp>

namespace Desert::Graphic::LedgerDetail
{
    uint64_t MintInstanceId( const ResourceInstanceKind kind )
    {
        static std::atomic<uint64_t> scenes{ 0 };
        static std::atomic<uint64_t> views{ 0 };
        return ( kind == ResourceInstanceKind::Scene ? scenes : views ).fetch_add( 1 ) + 1;
    }

    ResourceOwnerInstance& AmbientInstance()
    {
        static thread_local ResourceOwnerInstance instance;
        return instance;
    }

    ResourceOwner& AmbientOwner()
    {
        static thread_local ResourceOwner owner = ResourceOwner::Unclaimed;
        return owner;
    }

    uint64_t& NextRowId()
    {
        static uint64_t next = 1; // 0 is "accounts for nothing" and is never handed out
        return next;
    }

    LedgerState& State()
    {
        static LedgerState state;
        return state;
    }

    std::mutex& Lock()
    {
        static std::mutex lock;
        return lock;
    }
}
