#pragma once

// HAIR (GROOM) ASSET DATA — strands, guides, and the binding of their roots to a skinned mesh.
//
// UE counterpart: UGroomAsset with its hair groups (FHairGroupData: rendering strands + simulation guides,
// FHairStrandsDatas / FHairStrandsInterpolationDatas) and UGroomBindingAsset (roots projected onto a
// skeletal mesh's triangles) — HairStrandsCore/Public/GroomAsset.h, GroomBindingAsset.h, HairStrandsDatas.h.
// Pattern, not letter: plain std/glm arrays in structure-of-arrays form, centimetres, and one binding per
// (groom, mesh) pair exactly as UE keeps it — the same groom is bound to several heads.
//
// Why this is its own directory (Engine/Hair) and not under Physics: a groom is mostly RENDER data
// (thousands of strands) with a small simulated subset (the guides); the simulation is one consumer of it,
// the renderer the other. Cloth is the reverse — the simulated mesh IS the asset — so it lives in Physics.
//
// NOT HERE: HAIR1 — the asset type and file format, the importer (Alembic/USD curves), the binding builder
// (root projection onto the mesh), the guide solver and the strand renderer.

#include <Common/Core/AssetHandle.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Desert::Hair
{
    // A set of curves in structure-of-arrays form (UE FHairStrandsPoints + FHairStrandsCurves).
    // Points are in groom space, cm. Curve c owns points [CurvePointOffset[c], +CurvePointCount[c]); point 0
    // of every curve is its ROOT, the end attached to the scalp.
    struct HairCurves
    {
        std::vector<glm::vec3> Points;
        std::vector<float>     PointRadius; // cm
        std::vector<uint32_t>  CurvePointOffset;
        std::vector<uint32_t>  CurvePointCount;
        std::vector<glm::vec2> CurveRootUV;
    };

    // Each render strand follows up to three guides (UE FHairStrandsInterpolationDatas). Weights sum to 1.
    struct HairGuideInterpolation
    {
        std::vector<std::array<uint32_t, 3>> GuideCurve; // per render curve
        std::vector<std::array<float, 3>>    GuideWeight;
    };

    struct HairGroup
    {
        std::string            Name;
        HairCurves             Strands; // rendered, never simulated
        HairCurves             Guides;  // simulated, never rendered
        HairGuideInterpolation Interpolation;
        Common::AssetHandle    Material;
    };

    struct GroomAsset
    {
        std::string            Name;
        std::vector<HairGroup> Groups;
    };

    // Where a curve's root sits on the mesh: a triangle and barycentric coordinates on it, so the root
    // follows the skinned surface (UE: the root-to-triangle projection stored in the binding).
    struct HairRootAttachment
    {
        uint32_t  Triangle    = 0;
        glm::vec3 Barycentric = { 1.0f, 0.0f, 0.0f };
    };

    struct GroomBinding
    {
        Common::AssetHandle Groom;
        Common::AssetHandle SkinnedMesh;
        uint32_t            LodIndex = 0;
        // [group][curve], parallel to HairGroup::Guides and HairGroup::Strands.
        std::vector<std::vector<HairRootAttachment>> GuideRoots;
        std::vector<std::vector<HairRootAttachment>> StrandRoots;
    };
} // namespace Desert::Hair
