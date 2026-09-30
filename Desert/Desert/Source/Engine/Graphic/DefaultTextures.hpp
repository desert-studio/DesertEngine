#pragma once

#include <Engine/Core/Formats/DefaultTexture.hpp>
#include <Engine/Graphic/DynamicResources.hpp>
#include <Engine/Graphic/Image.hpp>

#include <array>

namespace Desert::Graphic
{
    /**
     * @brief The pixel behind each `Core::Formats::DefaultTextureKind` — what a sampler shows when a
     *        material binds nothing to it.
     *
     * WHAT THIS IS NOT. It is not `FallbackTextures`, which stands next to it and answers a different
     * question. A fallback exists so that a descriptor the engine never wrote is still DEFINED — its
     * colour is an implementation detail nobody authors and nobody should read meaning into. These are
     * AUTHORED: a shader says `Texture2D u_NormalTexture ("Normal Map") = "normal"`, and that sentence
     * is a statement about what the surface looks like with no map assigned. The two happen to agree on
     * white and disagree on everything else, which is exactly why they are not one table.
     *
     * WHY IT EXISTS AT ALL: without it a texture slot cannot be CLEARED. `SetImage` overwrites the bound
     * image and there is no image meaning "none", so before М9 clearing a slot emptied the `.demat` and
     * left the descriptor pointing at the last texture assigned — the file and the picture disagreeing
     * with nothing in between to notice (a Д31-class divergence). Binding one of these is what "none"
     * IS.
     *
     * BACKEND-NEUTRAL BY CONSTRUCTION, and deliberately so: `Image2D::Create` already dispatches on the
     * API, so this needs no per-backend subclass the way FallbackTextures does, and adding one would
     * mean editing `API/Vulkan` to say a fact about colour.
     *
     * LIFETIME. Built lazily on first use (a 1x1 upload needs a live device, and materials are the first
     * thing to ask), released explicitly from `Renderer::Shutdown()` beside the fallbacks — a static
     * destructor cannot be ordered against the device.
     */
    class DefaultTextures final : public DynamicResources
    {
    public:
        static DefaultTextures& Get();

        DefaultTextures( const DefaultTextures& )            = delete;
        DefaultTextures& operator=( const DefaultTextures& ) = delete;

        /**
         * @brief The 1x1 image @p kind stands for, creating the table on the first call.
         * @return nullptr only if the image could not be created, and then it has already been logged
         *         with the kind's name — never a quiet substitution of another kind's pixel.
         */
        const Image2D* Resolve( Core::Formats::DefaultTextureKind kind );

        /**
         * @brief Resolve, handing out the owning pointer: for a frame graph that imports the image as an
         *        external (FrameTextures::Import), e.g. the black texture a consumer binds when the
         *        producer of a transient did not run this frame.
         */
        std::shared_ptr<Image2D> Share( Core::Formats::DefaultTextureKind kind );

        [[nodiscard]] Common::BoolResultStr Invalidate() override
        {
            return BOOLSUCCESS;
        }
        [[nodiscard]] Common::BoolResultStr Release() override;

    private:
        DefaultTextures() = default;

        // Indexed by the enum's own value, so a kind added to the enum is a compile-time widening of
        // this array rather than a lookup that finds nothing at runtime.
        std::array<std::shared_ptr<Image2D>, std::size( Core::Formats::kAllDefaultTextureKinds )> m_Images;
    };
} // namespace Desert::Graphic
