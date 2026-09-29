#pragma once

#include <Engine/Assets/AssetRootSet.hpp>

#include <Common/Core/Core.hpp>

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace Desert::Assets
{
    /**
     * @brief A root the eviction sweep keeps that no scene names: an asset an open editor window is editing.
     *
     * The sweep traces from the live scenes' components (CollectAssetRootsFromLiveScenes). An asset editor's
     * subject is in no scene — the Animation Editor's clip is played by a preview entity only while the preview
     * exists — so a sweep released it mid-edit and an unsaved notify edit was lost to the reload from disk
     * (ANV1b2 frame: every notify on tick 0). A pin is held by the window for as long as it is open; the
     * collector marks every live pin with its reason. RAII, so a closed window cannot leave a pin behind.
     */
    class AssetRootPin final
    {
    public:
        AssetRootPin( const Common::AssetHandle& handle, std::string why ) : m_Handle( handle )
        {
            std::scoped_lock lock( Mutex() );
            m_Id = ++NextId();
            Pins().emplace( m_Id, std::make_pair( handle, std::move( why ) ) );
        }
        ~AssetRootPin()
        {
            std::scoped_lock lock( Mutex() );
            Pins().erase( m_Id );
        }
        AssetRootPin( const AssetRootPin& )            = delete;
        AssetRootPin& operator=( const AssetRootPin& ) = delete;

        [[nodiscard]] const Common::AssetHandle& Handle() const
        {
            return m_Handle;
        }

        static void MarkAll( AssetRootSet& roots )
        {
            std::scoped_lock lock( Mutex() );
            for ( const auto& [id, pin] : Pins() )
                roots.Mark( pin.first, pin.second );
        }

    private:
        static std::mutex& Mutex()
        {
            static std::mutex mutex;
            return mutex;
        }
        static uint64_t& NextId()
        {
            static uint64_t next = 0;
            return next;
        }
        static std::unordered_map<uint64_t, std::pair<Common::AssetHandle, std::string>>& Pins()
        {
            static std::unordered_map<uint64_t, std::pair<Common::AssetHandle, std::string>> pins;
            return pins;
        }

        Common::AssetHandle m_Handle;
        uint64_t            m_Id = 0;
    };
} // namespace Desert::Assets
