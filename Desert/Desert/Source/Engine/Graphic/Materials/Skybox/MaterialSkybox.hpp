#pragma once

#include <Engine/Graphic/Materials/Material.hpp>

#include <Engine/Assets/Skybox/SkyboxAsset.hpp>
#include <Engine/Graphic/Materials/MaterialExecutor.hpp>
#include <Engine/Graphic/Environment/SceneEnvironment.hpp>
#include <Engine/Graphic/Environment/SkyLook.hpp>

#include <Engine/Core/Camera.hpp>

namespace Desert::Graphic
{
    struct UpdateMaterialSkyboxInfo
    {
        Core::Camera* Camera;
    };

    class MaterialSkybox final : public Material
    {
    public:
        explicit MaterialSkybox( const std::shared_ptr<Assets::SkyboxAsset>& baseAsset );

        std::shared_ptr<Assets::SkyboxAsset> GetBaseMaterial() const
        {
            if ( auto material = m_BaseMaterial.lock() )
                return material;
            return nullptr;
        }

        const Environment& GetEnvironment() const { return m_Environment; }

        bool IsUsingBaseMaterial() const { return m_BaseMaterial.lock() != nullptr; }

        bool IsReady() const
        {
            if ( const auto& base = m_BaseMaterial.lock() )
                return base->IsReadyForUse();
            return false;
        }

        void BindInputs( const UpdateMaterialSkyboxInfo& data );

        /**
         * @brief Make the three cubes describe @p look, rebuilding them if they do not.
         *
         * RETURNS WHETHER IT REBUILT, because the caller is the one that knows what a rebuild costs the
         * frame it lands in and is the only place that can say so in the log with the numbers.
         *
         * THE DEVICE IS IDLE FOR THE WHOLE CALL when it rebuilds — the bake is the immediate,
         * submit-and-wait compute path — so it must not be called from inside the render graph. The
         * skybox render system calls it from its pre-graph slot, the same position the procedural sky's
         * own rebake occupies.
         */
        bool EnsureBaked( const SkyLook& look );

        /// The look the current cubes were baked from. Not the look that was last ASKED for: a bake that
        /// failed leaves the previous cubes standing, and reporting the request would make a failure look
        /// like a success for the rest of the session.
        [[nodiscard]] const SkyLook& BakedLook() const
        {
            return m_BakedLook;
        }

    private:
        /// Drop the three cubes of @p previous from the image service. Called when a rebake has replaced
        /// them: three RGBA32F cubes are ~120 MiB, and an HDR sky whose rotation is dragged through ten
        /// values would otherwise hold ten sets of them for the rest of the session.
        static void ReleaseEnvironment( const Environment& previous );

        std::weak_ptr<Assets::SkyboxAsset> m_BaseMaterial;
        std::shared_ptr<MaterialExecutor>  m_Material;
        Environment                        m_Environment;
        SkyLook                            m_BakedLook{};

        TextureCubeProperty* m_CubeMapTexture = nullptr;
    };
} // namespace Desert::Graphic
