#pragma once

#include "MaterialPBRBase.hpp"

#include <Engine/Assets/Mesh/PBRSurfaceParams.hpp>
#include <Engine/Core/Formats/MaterialParamRow.hpp>
#include <Engine/Graphic/Materials/Mesh/MeshVertexPath.hpp>

#include <glm/glm.hpp>

#include <memory>

namespace Desert::Graphic
{
    // THE runtime PBR material: one surface, drawn on whichever vertex path the geometry needs.
    //
    // It replaces five classes that were one surface each time — `StaticMaterialPBR`,
    // `StaticMaterialPBRInstanced`, `SkinnedMaterialPBR`, `MaterialGlass` and `MaterialRSM`. They
    // differed in exactly three things: the shader they named, whether they uploaded bone matrices, and
    // (for the skinned one) whether the per-object GPU material came from a batch or from the material's
    // own data. The first is now `MeshShaderFor(path, pass)`; the second is `UploadBones`, which only the
    // skinned path calls; the third was a difference that should never have existed, and its cost was
    // that a skinned mesh silently ignored every per-instance material override in the engine.
    //
    // Its parameters live entirely in the reflected `Assets::PBRSurfaceParams` — no per-parameter members
    // and no setters. MaterialFactory copies them from the material asset, the editor edits them through
    // reflection, and the renderer packs them into the `Materials[]` storage buffer.
    //
    // OWNERSHIP OF PER-FRAME STATE. A material is shared by every entity that uses that `.demat` on that
    // path, so nothing PER-OBJECT may be stored on it beyond the moment of one Bind: the transform and
    // the material index ride push constants (which Vulkan snapshots per draw), and the bone matrices
    // ride ONE storage buffer holding every pose drawn with this material this frame, indexed by a
    // per-draw offset. Storing a pose on the material instead is what made two skinned meshes render as
    // one — the second Bind overwrote the buffer the first draw's descriptor still pointed at, and both
    // recorded draws then executed against the second pose.
    class MaterialPBR final : public MaterialPBRBase
    {
    public:
        // `pass` selects what the fragment stage writes; the shader comes from the PAIR. A pair the
        // engine has no shader for (see the holes in MeshShaderFor's table) yields NULL and one
        // LOG_ERROR naming the pair — never a quiet fall back to the static forward shader, which would
        // render a skinned mesh in its bind pose and look like an animation bug.
        static std::shared_ptr<MaterialPBR> Create( MeshVertexPath path, MeshPass pass = MeshPass::Forward );

        ~MaterialPBR() override = default;

        MeshVertexPath VertexPath() const
        {
            return m_Path;
        }
        MeshPass Pass() const
        {
            return m_Pass;
        }

        Assets::PBRSurfaceParams& Data()
        {
            return m_Data;
        }
        const Assets::PBRSurfaceParams& Data() const
        {
            return m_Data;
        }

        // The shader's `Properties Binding(2)` manifest and this material's row of it — the SAME transport
        // DataDrivenMaterial uses (Core/Formats/MaterialParamRow.hpp). MaterialFactory builds the row from the
        // asset's persisted params; the renderer copies it (plus instance overrides, by name) into
        // `Materials[]`. Every PBR pass declares the one layout, so a forward row feeds GBuffer/glass as is.
        [[nodiscard]] const Core::Formats::ShaderProgramMeta& GetSchema() const
        {
            return m_Schema;
        }
        [[nodiscard]] const Core::Formats::MaterialParamRow& GetParamRow() const
        {
            return m_Row;
        }
        void SetParamRow( Core::Formats::MaterialParamRow row )
        {
            m_Row = std::move( row );
        }

        // Which row of `Materials[]` the next draw reads is Material::SetMaterialIndex — the one entry
        // point every transport in the engine now shares. It used to be a member here plus a push in
        // Bind, and the push had to be repeated by anything that drew without an instance.

        // Where this draw's bones start in the packed `Bones` buffer below. Skinned path only.
        void SetBoneOffset( uint32_t firstBone )
        {
            m_BoneOffset = firstBone;
        }

        // Upload every pose that will be drawn with this material in this pass, ONCE, packed end to end.
        // Each draw then names its own slice through SetBoneOffset. Skinned path only; calling it on any
        // other path is a bug in the renderer, not bad data, so it verifies rather than returning a
        // result.
        void UploadBones( const glm::mat4* matrices, size_t count );

        // The per-OBJECT model matrix. Still on the instance (and therefore still a push constant)
        // because it is genuinely per object; the five per-FRAME uploads that used to sit beside it are
        // PBRSceneFrame::ApplyTo's job and reach every material through SceneLightingBinding.hpp.
        static void UpdateTransform( MaterialInstance* instance, const glm::mat4& transform );

        void Bind( const MaterialInstance* instance ) override;

    private:
        MaterialPBR( MeshVertexPath path, MeshPass pass, const char* shaderName );

        Assets::PBRSurfaceParams         m_Data;
        Core::Formats::ShaderProgramMeta m_Schema;
        Core::Formats::MaterialParamRow  m_Row;
        uint32_t                 m_BoneOffset = 0;
        MeshVertexPath           m_Path;
        MeshPass                 m_Pass;
    };
} // namespace Desert::Graphic
