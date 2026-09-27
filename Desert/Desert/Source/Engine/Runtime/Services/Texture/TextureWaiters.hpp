#pragma once
// Which materials drew a slot default because a texture was still Pending, and who gets rebuilt when it
// lands. Its own header with no Vulkan behind it, so a test can reach the RELATION (a texture becoming
// Ready invalidates exactly the materials that waited on it, once) — TextureService and MaterialService
// themselves pull in the renderer and cannot be linked into one.
#include <Engine/Assets/Common.hpp>

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace Desert::Runtime
{
    class TextureWaiters
    {
    public:
        /// @p material bound a default for @p texture. Recorded once however many slots or cells asked.
        void Add( const Assets::AssetHandle& texture, const Assets::AssetHandle& material )
        {
            auto& waiting = m_Waiting[texture];
            if ( std::find( waiting.begin(), waiting.end(), material ) == waiting.end() )
                waiting.push_back( material );
        }

        /// @p texture settled (Ready or Failed): every material that waited on it is handed to
        /// @p invalidate exactly once, and forgotten. Failed settles too — the waiter would otherwise keep
        /// a default bound for a texture that will never arrive, with no rebuild to say so.
        template <typename Invalidate>
        void Settle( const Assets::AssetHandle& texture, Invalidate&& invalidate )
        {
            const auto it = m_Waiting.find( texture );
            if ( it == m_Waiting.end() )
                return;
            const auto waiting = std::move( it->second );
            m_Waiting.erase( it );
            for ( const auto& material : waiting )
                invalidate( material );
        }

        void Clear()
        {
            m_Waiting.clear();
        }

    private:
        std::unordered_map<Assets::AssetHandle, std::vector<Assets::AssetHandle>> m_Waiting;
    };
} // namespace Desert::Runtime
