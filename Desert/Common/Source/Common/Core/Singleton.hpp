#pragma once

#include <Common/Core/Core.hpp>

#include <memory>
#include <utility>

namespace Common
{
    template <typename T>
    class Singleton
    {
    public:
        static T& GetInstance()
        {
            DESERT_VERIFY( m_Instance, "a singleton is read before CreateInstance" );
            return *m_Instance;
        }

        template <typename... Args>
        static T& CreateInstance( Args&&... args )
        {
            if ( !m_Instance )
            {
                m_Instance = std::make_unique<T>( std::forward<Args>( args )... );
            }
            return *m_Instance;
        }

    protected:
        Singleton( const Singleton& )            = delete;
        Singleton& operator=( const Singleton& ) = delete;
        Singleton( Singleton&& )                 = delete;
        Singleton& operator=( Singleton&& )      = delete;

        Singleton()          = default;
        virtual ~Singleton() = default;

    private:
        static std::unique_ptr<T> m_Instance;
    public:
        template <class T2>
        friend class Singleton;
    };

    template <typename T>
    std::unique_ptr<T> Singleton<T>::m_Instance = nullptr;
} // namespace Common
