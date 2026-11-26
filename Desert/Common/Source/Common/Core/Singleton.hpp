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
            std::unique_ptr<T>& instance = Instance();
            DESERT_VERIFY( instance, "a singleton is read before CreateInstance" );
            return *instance;
        }

        template <typename... Args>
        static T& CreateInstance( Args&&... args )
        {
            std::unique_ptr<T>& instance = Instance();
            if ( !instance )
            {
                instance = std::make_unique<T>( std::forward<Args>( args )... );
            }
            return *instance;
        }

    protected:
        Singleton( const Singleton& )            = delete;
        Singleton& operator=( const Singleton& ) = delete;
        Singleton( Singleton&& )                 = delete;
        Singleton& operator=( Singleton&& )      = delete;

        Singleton()          = default;
        virtual ~Singleton() = default;

    private:
        // The one storage of the instance. No generic definition: the owner declares the explicit specialization
        // right after its class and defines it in its own .cpp with a function-local static, so every module DLL
        // reaches the owner's single copy through a function (a header-defined static gives each DLL its own copy;
        // design BINT-STATE). A TU that misses the declaration fails at link, never silently.
        static std::unique_ptr<T>& Instance();
    public:
        template <class T2>
        friend class Singleton;
    };
} // namespace Common
