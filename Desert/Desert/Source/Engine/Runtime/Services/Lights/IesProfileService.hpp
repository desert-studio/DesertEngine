#pragma once

#include <Engine/Graphic/IesProfile.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Graphic
{
    class Image2D;
}

namespace Desert::Runtime
{
    /**
     * @brief THE IES PROFILES THE LOCAL LIGHTS NAME, packed into ONE atlas texture (UE FIESAtlas): each profile a
     * block of Graphic::kIesProfileRows rows of the R32F image, the block's first row the profile's index in every
     * light payload (Point/Spot IesProfileRow), the image's bindless slot its IesAtlasTexture. Point, spot and
     * rect lights sample it in Mesh/IesProfile.glslh by the direction from the light in the light's own frame.
     *
     * TWO HALVES ON TWO THREADS. The light collectors run in parallel (ECS::*LightECSSystem::CanRunParallel):
     * they call AcquireRow, which reads and resamples a profile on its first ask (a few kB of text, once) and is
     * guarded. The renderer's command replay (SceneRenderer::AddPointLight / AddSpotLight, the main thread) calls
     * AtlasTexture, which builds the image when a profile was added since - so a new profile shows from the frame
     * that asked for it, and no image is ever made off the main thread.
     */
    class IesProfileService
    {
    public:
        // No profile: the payload value the shaders read as "unshaped" (IesProfile.glslh).
        static constexpr int32_t kNoProfile = -1;

        // The first atlas row of the profile at @p path (a .ies file, as the light component names it), reading
        // it on first ask; kNoProfile for an empty path. A profile that cannot be read or resampled is logged once
        // and answers kNoProfile from then on (the light then emits as without one, and the log says why).
        [[nodiscard]] int32_t AcquireRow( std::string_view path );

        // The atlas image's bindless slot, (re)building the image first if AcquireRow added a profile since the
        // last call; 0 while no profile has been read. Main thread only.
        [[nodiscard]] int32_t AtlasTexture();

        void Clear();

    private:
        struct Entry
        {
            int32_t Row = kNoProfile;
        };

        std::mutex                                m_Mutex;
        std::map<std::string, Entry, std::less<>> m_ByPath;
        std::vector<float>                        m_Texels; // every profile's block, in row order
        uint32_t                                  m_Profiles = 0;
        bool                                      m_Dirty    = false;
        std::shared_ptr<Graphic::Image2D>         m_Atlas;
        // Atlases replaced by a rebuild. Payloads already recorded for frames in flight name their slot, so they
        // are held until Clear (a rebuild happens once per profile added, so this is a handful per session).
        std::vector<std::shared_ptr<Graphic::Image2D>> m_Replaced;
    };
} // namespace Desert::Runtime
