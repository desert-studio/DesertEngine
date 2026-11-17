#pragma once

#include <cstdint>
#include <memory>
#include <mutex>

namespace Desert::Graphic
{
    class Image2D;
}

namespace Desert::Runtime
{
    /**
     * @brief THE RECT LIGHTS' GGX LTC TABLES (UE FSystemTextures::LTCMat / LTCAmp, sampled by RectGGXApproxLTC in
     * RectLight.ush): the linearly-transformed-cosine fit of the GGX lobe (Heitz et al. 2016) that integrates a
     * rect light's specular over the rect's polygon (Mesh/RectLight.glslh EvaluateRectLightSpecular).
     *
     * ONE RGBA32F image, 64 x 128 texels: rows 0..63 the inverse LTC matrix, rows 64..127 its amplitude (the
     * Fresnel scale and bias), both over (perceptual roughness, sqrt( 1 - NoV )). The data is constant
     * (RectLightLtcTable.inl), so the image is made once, on the first rect light the renderer meets, and its
     * bindless slot stamped into that light's payload (SceneRenderer::AddPointLight, PointLightPayload::
     * RectLtcTexture) - the way the IES atlas reaches every pass that lights by DesertPointLightAt.
     */
    class RectLightLtcService
    {
    public:
        // The tables' bindless slot, creating the image on the first call; 0 when it could not be created
        // (logged once: the rect lights then reflect no specular, and the log says why). Main thread only.
        [[nodiscard]] int32_t LtcTexture() noexcept;
        void                  Clear() noexcept;

    private:
        std::mutex                        m_Mutex;
        std::shared_ptr<Graphic::Image2D> m_Table;
        bool                              m_Refused = false;
    };
} // namespace Desert::Runtime
