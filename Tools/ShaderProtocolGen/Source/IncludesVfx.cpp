#include "IncludeLists.hpp"

#include <VFX/Public/ParticleLayout.hpp>

namespace Desert::ShaderProtocolGen
{
    // Run C: the particle programs' records (ParticleSimulate, ParticleDispatchArgs, the pool and draw layout).
    std::span<const ShaderIncludeDecl* const> VfxIncludes()
    {
        static constexpr const ShaderIncludeDecl* kIncludes[] = {
             &Graphic::System::kParticleShaderInclude,
        };
        return kIncludes;
    }
} // namespace Desert::ShaderProtocolGen
