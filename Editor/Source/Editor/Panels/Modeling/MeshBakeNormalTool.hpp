#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Editor/Core/Selection/BakeNormalMapSettings.hpp>

#include <filesystem>
#include <string>

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Editor::Core
{
    // UE's Bake Normal Map (Modeling Mode -> Bake -> BakeTx, UBakeMeshAttributeMapsTool with Map Type = Tangent
    // Normal; MeshModelingToolsExp BakeMeshAttributeMapsTool.cpp) on the TWO entities of the scene selection: the
    // FIRST selected is the target (the low-poly whose UVs the map is laid out in, UE's Targets[0]), the SECOND
    // the detail (the high-poly whose normals are baked, UE's detail mesh). The bake itself is
    // Geometry::BakeMeshMaps, in world space.
    //
    // THE RESULT IS AN ASSET, as UE's Accept writes a UTexture2D: the map is written as an image under
    // `Assets/Textures/Baked/`, imported into its texture asset with Intent = NormalMap (cooked BC5,
    // TextureIntent.hpp), and a material that binds it in its template's normal-map slot is assigned to every
    // slot of the target, one undo step. The material is an INSTANCE of the target's slot-0 material when that
    // slot names one, so the target keeps its look and gains only the normal map; with no material it is a new
    // material on the project's Default Surface template. In UE the tool has a live preview and Accept; here it
    // runs on one click, as Boolean does (Core/Selection/MeshBooleanTool.hpp).
    //
    // Ambient Occlusion and Curvature (UE's other Bake Mesh Attribute Maps, Settings.Occlusion / .Curvature) are
    // written beside the normal map as one-channel textures `<target>_AO` and `<target>_Curvature` (Intent =
    // Mask); the occlusion is also packed into `<target>_ORM` (R = AO, G = B = 1) and bound in the template's
    // PackedMasks slot, so the target shows it at once.
    // The arguments and the palette's settings are Core's (BakeNormalMapSettings.hpp): the modeling property
    // table reads them, and this tool, which imports textures, sits in the panel layer above it.

    struct BakeNormalMapOutcome
    {
        std::filesystem::path TextureAsset; // the .detex
        std::filesystem::path Material;     // the .demat assigned
        std::filesystem::path OcclusionAsset; // the AO .detex, empty unless Settings.Occlusion
        std::filesystem::path CurvatureAsset; // the Curvature .detex, empty unless Settings.Curvature
        std::size_t           CoveredTexels = 0;
        std::size_t           HitTexels     = 0;
    };

    // Refused, with the scene and the disk untouched where the refusal comes before the write: a selection of
    // other than two entities; an entity without a static mesh, a transform or a readable mesh; a target without
    // UVs or a tangent frame; every refusal of BakeMeshMaps (no overlap within the projection distance
    // named with its numbers); a template with no single normal-map slot; an image, asset or material that did not
    // reach the disk.
    [[nodiscard]] Common::ResultStr<BakeNormalMapOutcome> ApplyBakeNormalMap( ::Desert::Core::Scene&   scene,
                                                                              Assets::AssetManager&    assets,
                                                                              const BakeNormalMapArgs& args );
} // namespace Desert::Editor::Core
