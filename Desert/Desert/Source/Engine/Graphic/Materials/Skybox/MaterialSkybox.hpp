#pragma once

#include <Engine/Graphic/Materials/Material.hpp>

#include <Engine/Assets/Skybox/SkyboxAsset.hpp>
#include <Engine/Graphic/Materials/MaterialExecutor.hpp>
#include <Engine/Graphic/Environment/SceneEnvironment.hpp>
#include <Engine/Graphic/GpuBatch.hpp>
#include <Engine/Graphic/Environment/SkyLook.hpp>

#include <Engine/Core/Camera.hpp>

namespace Desert::Graphic
{
    struct UpdateMaterialSkyboxInfo
    {
        Core::Camera* Camera;
        // The look of the SCENE drawing this sky. It travels per draw rather than living on the material
        // because the material is one per `.hdr` and shared by every view that names it — two scenes at
        // two rotations used to rebake it alternately; now each simply binds its own value.
        SkyLook Look{};
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

        /// Is the GPU still convolving this skybox's cubes (a cache miss, AL1-3c)? Never blocks. While it
        /// answers yes nothing may sample the environment; SkyboxService keeps the skybox pending and
        /// hands the material out only after `SettleConvolution`.
        [[nodiscard]] bool IsConvolving() const { return m_Convolving && !m_Convolving->IsComplete(); }

        /// Lets go of the finished convolution batch and what it retained. Only after IsConvolving() said no.
        void SettleConvolution() { m_Convolving.reset(); }

        bool IsUsingBaseMaterial() const { return m_BaseMaterial.lock() != nullptr; }

        bool IsReady() const
        {
            if ( const auto& base = m_BaseMaterial.lock() )
                return base->IsReadyForUse();
            return false;
        }

        void BindInputs( const UpdateMaterialSkyboxInfo& data );

    private:
        std::weak_ptr<Assets::SkyboxAsset> m_BaseMaterial;
        std::shared_ptr<MaterialExecutor>  m_Material;
        Environment                        m_Environment;
        std::unique_ptr<GpuBatch>          m_Convolving;

        TextureCubeProperty* m_CubeMapTexture = nullptr;
    };
} // namespace Desert::Graphic
