#pragma once

#include <Engine/Assets/MaterialAsset.hpp>

#include <Engine/Graphic/Materials/Material.hpp>
#include <Engine/Graphic/Materials/Mesh/MeshVertexPath.hpp>

namespace Desert::Assets
{
    class SurfaceMaterialAsset;
}

namespace Desert::Graphic
{
    class MaterialPBR;
    class DataDrivenMaterial;

    class MaterialFactory
    {
    public:
        // Create material from asset (pure material, no instance data) FOR A GIVEN VERTEX PATH AND PASS.
        //
        // Neither is a property of the asset, which is the whole point: a `.demat` describes a surface and
        // says nothing about how the geometry under it is fetched or what the fragment stage writes. The
        // same asset therefore yields a static material and a skinned material with the same parameters,
        // and they cannot drift because there is one asset behind both. Before this, the answer was always
        // the static class — including for an asset that explicitly named the skinned shader — and a
        // skinned mesh with an authored material was dropped by the renderer without drawing.
        //
        // The PASS is here for the same reason the path is. A material is "one shader's descriptor sets
        // plus a parameter payload", and the shader is `MeshShaderFor(path, pass)` — so a pass with no
        // material of its own has no descriptor sets of its own and must borrow a neighbour's, which is
        // only legal while the two shaders declare byte-identical layouts. The deferred G-buffer pass did
        // exactly that, and StaticMeshGBuffer.shader carried fourteen bindings it never read to keep the
        // borrow legal. It has its own material now.
        //
        // A custom-shader (`DataDrivenMaterial`) asset has no skinned, instanced or G-buffer variant to
        // build: DSL surface shaders carry no skinning stage and no deferred variant. Asking for one
        // returns NULL and logs the material's name once, rather than handing back a material whose vertex
        // stage would render the bind pose.
        static std::shared_ptr<Material> CreateMaterial( const Assets::MaterialAsset* asset,
                                                         MeshVertexPath path = MeshVertexPath::Static,
                                                         MeshPass       pass = MeshPass::Forward );

        // THE ONE RULE of which (path x pass) cells a surface template has: the PBR surface template has every
        // cell; a custom DSL surface shader has only (Static x Forward). CreateMaterial refuses by it, and a
        // caller that must know BEFORE it stages a draw (the skinned thumbnail, THM1n-10) asks it through
        // MaterialService::CellOf rather than learning it from a substituted default material. @p asset is a
        // BASE material (an instance's cells are its base's). The error names the material, shader and cell.
        static Common::BoolResultStr HasCell( const Assets::MaterialAsset& asset, MeshVertexPath path,
                                              MeshPass pass );

        // Copy a PBR asset's reflected data into a live runtime material and (re)bind its textures.
        // Used by CreateMaterial and by the editor for live edit -> viewport sync.
        static void ApplyPBRAsset( MaterialPBR& material, const Assets::SurfaceMaterialAsset& asset );

        // Apply a custom-shader material asset (ShaderName/ShaderParams/ShaderTextures) onto its
        // runtime DataDrivenMaterial. Counterpart of ApplyPBRAsset for the generic path.
        static void ApplyShaderAsset( DataDrivenMaterial& material, const Assets::SurfaceMaterialAsset& asset );

        // CreateMaterialInstance / CreateDefaultPBRMaterial / CreatePrimitiveMaterial stood here and had
        // no caller anywhere in the engine, the editor or the tests. They are gone rather than given a
        // path parameter: a factory entry point nobody calls is a second way to build a material that no
        // frame ever exercises, which is exactly how the static and skinned classes drifted apart.
    };
} // namespace Desert::Graphic