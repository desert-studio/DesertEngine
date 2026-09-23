#pragma once

#include <algorithm>
#include <vector>

namespace Common
{
    template <typename Derived>
    class AutoRegistry
    {
        inline static std::vector<Derived*> m_Instances;

    public:
        AutoRegistry( const AutoRegistry& )            = delete;
        AutoRegistry& operator=( const AutoRegistry& ) = delete;
        AutoRegistry( AutoRegistry&& )                 = delete;
        AutoRegistry& operator=( AutoRegistry&& )      = delete;

        AutoRegistry()
        {
            m_Instances.push_back( static_cast<Derived*>( this ) );
        }

        ~AutoRegistry()
        {
            auto it = std::find( m_Instances.begin(), m_Instances.end(), static_cast<Derived*>( this ) );
            if ( it != m_Instances.end() )
                m_Instances.erase( it );
        }

        template <typename Callback>
        static void ForEach( Callback func )
        {
            for ( Derived* obj : m_Instances)
                func( *obj );
        }
    };
} // namespace Common