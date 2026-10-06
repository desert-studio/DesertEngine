#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>

namespace Desert::Editor::UI
{
    /**
     * @brief WHICH UI TEXTURE ID BELONGS TO WHICH LIVE IMAGE — keyed by the image's OWNER, never by its view.
     *
     * The cache this replaces was `unordered_map<VkImageView, ImTextureID>`, never cleared. A view handle is
     * a name Vulkan recycles once the view is destroyed, so after an image was released (a texture evicted,
     * a viewport resized, a sampler recreated) the map could hand ImGui a descriptor set written against a
     * dead view and sampler — and every set it ever made stayed allocated in the ImGui pool for the session.
     * That is why `AssetEviction.hpp` used to refuse to release any `Graphic::Image`.
     *
     * THE ENTRY IS THE OWNER'S, NOT THE HANDLE'S. Each entry holds a `weak_ptr` to the image that asked for
     * it and the image's RESOURCE GENERATION (bumped whenever its view or sampler is recreated). An entry is
     * answered only while its image is alive AND has the generation the set was written with; anything else
     * is stale and is handed to `retire`, which frees it once no frame in flight can still sample it (UE's
     * deferred deletion: the caller queues the free on the frame number, see `UICacheTextureImGui`).
     * `Sweep` does the same for images that died without asking again — every frame, before the UI is built.
     *
     * The address key is safe because it is never trusted alone: a live `weak_ptr` proves the object at that
     * address is the one the entry was made for (two live objects cannot share an address), and an expired
     * one makes the entry stale whatever now lives there.
     *
     * Header-only, free of Vulkan and ImGui, so `Desert/Tests/Engine/AssetEviction` holds the rule against a
     * stand-in image without a device.
     */
    template <typename Image, typename Id>
    class ImageTextureIds final
    {
    public:
        /// The id for @p image at resource generation @p generation: the cached one while it is still the
        /// image's, otherwise the stale one goes to @p retire and @p make writes a new one. A null id from
        /// @p make is returned and not cached.
        template <typename Make, typename Retire>
        Id Acquire( const std::shared_ptr<Image>& image, std::uint64_t generation, Make&& make, Retire&& retire )
        {
            if ( !image )
                return Id{};
            const auto it = m_Entries.find( image.get() );
            if ( it != m_Entries.end() )
            {
                if ( !it->second.Owner.expired() && it->second.Generation == generation )
                    return it->second.Value;
                retire( it->second.Value );
                m_Entries.erase( it );
            }
            const Id id = make();
            if ( id != Id{} )
                m_Entries.emplace( image.get(), Entry{ image, generation, id } );
            return id;
        }

        /// The image no longer has a view to show (released, not recreated): its entry goes to @p retire.
        template <typename Retire>
        bool Drop( const std::shared_ptr<Image>& image, Retire&& retire )
        {
            const auto it = image ? m_Entries.find( image.get() ) : m_Entries.end();
            if ( it == m_Entries.end() )
                return false;
            retire( it->second.Value );
            m_Entries.erase( it );
            return true;
        }

        /// Every entry whose image has been destroyed goes to @p retire. Returns how many.
        template <typename Retire>
        std::size_t Sweep( Retire&& retire )
        {
            std::size_t retired = 0;
            for ( auto it = m_Entries.begin(); it != m_Entries.end(); )
            {
                if ( !it->second.Owner.expired() )
                {
                    ++it;
                    continue;
                }
                retire( it->second.Value );
                it = m_Entries.erase( it );
                ++retired;
            }
            return retired;
        }

        /// Forget every entry WITHOUT retiring it: only for the owner of the pool that is about to be
        /// destroyed, which frees every set in it at once.
        void Forget() noexcept
        {
            m_Entries.clear();
        }

        /// The image has a live entry (tests, diagnostics).
        [[nodiscard]] bool Holds( const std::shared_ptr<Image>& image ) const
        {
            const auto it = image ? m_Entries.find( image.get() ) : m_Entries.end();
            return it != m_Entries.end() && !it->second.Owner.expired();
        }

        [[nodiscard]] std::size_t Size() const noexcept
        {
            return m_Entries.size();
        }

    private:
        struct Entry
        {
            std::weak_ptr<Image> Owner;
            std::uint64_t        Generation = 0;
            Id                   Value{};
        };
        std::unordered_map<const Image*, Entry> m_Entries;
    };
} // namespace Desert::Editor::UI
