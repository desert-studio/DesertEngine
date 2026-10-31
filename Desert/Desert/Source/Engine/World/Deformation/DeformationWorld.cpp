#include <Engine/World/Deformation/DeformationWorld.hpp>

#include <cmath>
#include <format>

namespace Desert::World
{
    Common::BoolResultStr DeformationWorld::Submit( DeformContact contact )
    {
        if ( contact.Stamp.empty() )
            return Common::MakeError( "deform: no stamp named (a DeformStamp shader's name)" );
        if ( !( contact.Params.Radius > 0.0f ) || !std::isfinite( contact.Params.Radius ) )
            return Common::MakeError( std::format( "deform: stamp '{}' radius {} cm is not a positive size",
                                                   contact.Stamp, contact.Params.Radius ) );
        if ( !std::isfinite( contact.At.x ) || !std::isfinite( contact.At.y ) || !std::isfinite( contact.At.z ) )
            return Common::MakeError( std::format( "deform: stamp '{}' position is not finite", contact.Stamp ) );
        contact.Sequence = m_Next++;
        m_Log.push_back( std::move( contact ) );
        while ( m_Log.size() > kMaxRetained )
            m_Log.pop_front();
        return BOOLSUCCESS;
    }

    std::vector<DeformContact> DeformationWorld::Since( const uint64_t from, uint64_t& missed ) const
    {
        missed = 0;
        std::vector<DeformContact> out;
        if ( m_Log.empty() || from >= m_Next )
            return out;
        const uint64_t first = m_Log.front().Sequence;
        if ( from < first )
            missed = first - from;
        for ( std::size_t i = from > first ? static_cast<std::size_t>( from - first ) : 0u; i < m_Log.size(); ++i )
            out.push_back( m_Log[i] );
        return out;
    }
} // namespace Desert::World
