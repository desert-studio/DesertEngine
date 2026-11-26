#include <Engine/Graphic/ShadowCascades.hpp>

namespace Desert::Graphic
{
    ShadowAttachmentLease::ShadowAttachmentLease( const ShadowQuality& quality, const uint32_t directionalLights )
         : m_Bytes( ShadowAttachmentBytes( quality, directionalLights ) )
    {
        if ( m_Bytes != 0 )
        {
            s_LiveBytes += m_Bytes;
            ++s_LiveHolders;
        }
    }

    auto ShadowAttachmentLease::LiveHolders() -> uint32_t
    {
        return s_LiveHolders;
    }

    auto ShadowAttachmentLease::LiveBytes() -> uint64_t
    {
        return s_LiveBytes;
    }

    auto ShadowAttachmentLease::Release() -> void
    {
        if ( m_Bytes == 0 )
            return;
        s_LiveBytes -= m_Bytes;
        --s_LiveHolders;
        m_Bytes = 0;
    }

    uint64_t ShadowAttachmentLease::s_LiveBytes = 0;

    uint32_t ShadowAttachmentLease::s_LiveHolders = 0;
}
