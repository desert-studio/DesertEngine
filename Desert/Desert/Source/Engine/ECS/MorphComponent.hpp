#pragma once

#include <entt/entt.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/UUID.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <CoreReflection/ReflectionMacros.hpp>

namespace Desert::ECS
{
    // Blendshape / morph-target weights for the entity's mesh. Weight k (0..1, though over/undershoot is
    // allowed) scales morph target k of the mesh asset; the blend is base + Σ(weight·delta) BEFORE skinning
    // (UE GPUMorphUpdateCS: Programs/Compute/MorphTargets.shader, run by MeshRenderer's morph update for a
    // skinned mesh; Geometry::ApplyMorphTargets is its CPU twin). TargetNames mirrors the mesh's target names
    // for the Details UI and stays index-aligned with the weights. Non-reflected (like AnimationComponent) —
    // edited via the Morph widget.
    //
    // The weights are CLOSED: they are written only through SetWeight / SetWeights (UE
    // USkinnedMeshComponent::SetMorphTarget), and the setter itself moves Revision when a value changes. Every
    // writer — the Details sliders, ReadComponent, and any future Lua / Sequencer binding — goes through it,
    // so no writer can forget to tell the renderer.
    struct MorphComponent
    {
        std::vector<std::string> TargetNames;

        const std::vector<float>& GetWeights() const;
        // Weight of target @p index, or 0 when the component holds no weight for it.
        float GetWeight( std::size_t index ) const;
        // Sets the weight of target @p index, growing the list with zeros if it is shorter.
        void     SetWeight( std::size_t index, float weight );
        void     SetWeights( std::vector<float> weights );
        uint64_t GetRevision() const;

    private:
        std::vector<float> Weights;

        // THE REVISION OF Weights (UE FMorphVertexBuffer's revision number): moved by the setters on every
        // change, and the renderer re-blends a component's morphed vertices only when the revision it blended
        // last differs - one integer compare per frame instead of a copy of the weights. Transient (not
        // serialized): a freshly read component has no blend yet, so the renderer blends it once whatever the
        // number is.
        uint64_t Revision = 0;
    };

    inline const std::vector<float>& MorphComponent::GetWeights() const
    {
        return Weights;
    }

    inline float MorphComponent::GetWeight( std::size_t index ) const
    {
        return index < Weights.size() ? Weights[index] : 0.0f;
    }

    inline void MorphComponent::SetWeight( std::size_t index, float weight )
    {
        if ( index >= Weights.size() )
            Weights.resize( index + 1, 0.0f );
        else if ( Weights[index] == weight )
            return;
        Weights[index] = weight;
        ++Revision;
    }

    inline void MorphComponent::SetWeights( std::vector<float> weights )
    {
        if ( weights == Weights )
            return;
        Weights = std::move( weights );
        ++Revision;
    }

    inline uint64_t MorphComponent::GetRevision() const
    {
        return Revision;
    }
} // namespace Desert::ECS
