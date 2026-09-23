#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <memory>

namespace Common::Memory
{
    const uint32_t MAX_COMMANDS_MEMORY = 12 * 1024 * 1024; // 12 MB

    class CommandBuffer
    {
    public:
        typedef void ( *CommandFunc )( void* );
        CommandBuffer()
        {
            m_CommandBufferPtr = m_CommandBuffer = new std::byte[MAX_COMMANDS_MEMORY];
            memset( m_CommandBuffer, 0, MAX_COMMANDS_MEMORY );
        }

        ~CommandBuffer()
        {
            delete[] m_CommandBuffer;
        }

        void* Allocate( CommandFunc func, uint32_t size )
        {
            *(CommandFunc*)m_CommandBufferPtr = func;
            m_CommandBufferPtr += sizeof( CommandFunc );

            *(uint32_t*)m_CommandBufferPtr = size;
            m_CommandBufferPtr += sizeof( uint32_t );

            void* memory = m_CommandBufferPtr;
            m_CommandBufferPtr += size;

            m_CommandCount++;
            return memory;
        }

        void Execute()
        {
            std::byte* buffer = m_CommandBuffer;

            for ( uint32_t i = 0; i < m_CommandCount; i++ )
            {
                CommandFunc func = *(CommandFunc*)buffer;
                buffer += sizeof( CommandFunc );

                uint32_t size = *(uint32_t*)buffer;
                buffer += sizeof( uint32_t );

                func( buffer );
                buffer += size;
            }

            m_CommandBufferPtr = m_CommandBuffer;
            m_CommandCount     = 0;
        }

    private:
        std::byte* m_CommandBuffer    = nullptr;
        std::byte* m_CommandBufferPtr = nullptr;

        uint32_t m_CommandCount = 0;
    };

    template <typename Func>
    void SubmitCommand( CommandBuffer* cmdBuffer, Func&& command )
    {
        auto func = []( void* ptr )
        {
            auto destFunc = static_cast<Func*>( ptr );
                 ( *destFunc )();

            destFunc->~Func();
        };

        auto storageBuffer = cmdBuffer->Allocate( func, sizeof( func ) );
        new ( storageBuffer ) Func( std::forward<Func>( command ) );
    }

} // namespace Common::Memory