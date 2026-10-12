#pragma once

#include <Engine/VFX/VFXWorld.hpp>

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::VFX
{
    /**
     * @brief The system lookup a host that owns an AssetManager gives each scene's VFXWorld (VFX-03d).
     *
     * A VFXComponent names its system by handle; the scene has no asset manager, so the host that builds the
     * scene's systems (editor SceneWorkspace::BuildSceneSystems, runtime RuntimeLayer::BuildGameplaySystems)
     * sets this once per scene. A handle the manager does not hold, or holds not yet loaded, answers nullptr
     * (VFXWorld reports it once and the entity spawns nothing). The manager must outlive the scene.
     */
    [[nodiscard]] VFXSystemLookup MakeAssetSystemLookup( const Assets::AssetManager& assets );
} // namespace Desert::VFX
