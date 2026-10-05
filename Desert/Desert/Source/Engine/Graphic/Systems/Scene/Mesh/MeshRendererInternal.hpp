#pragma once

// Private to the MeshRenderer*.cpp files (the class is defined across Shadow / Deferred / Forward / Debug
// parts): the include set they share and the two material helpers more than one part calls.
#include "MeshRenderer.hpp"
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/FrameGraphRefs.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Materials/MaterialExecutor.hpp>
#include <Engine/Graphic/ShadowCascades.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Graphic/Materials/Mesh/PBR/PBRPush.hpp>
#include <Engine/Core/Formats/MaterialParamRow.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>
#include <Engine/Geometry/LODSelection.hpp>
#include <Engine/Geometry/MeshBounds.hpp>
#include <Engine/Graphic/VisibilityCulling.hpp>
// MeshShaderFor / MeshVertexPath / MeshPass — the (path x pass) table this file asks for its pipelines.
#include <Engine/Graphic/Materials/Mesh/MeshVertexPath.hpp>
#include <Engine/Graphic/Materials/Mesh/InstancedRecorder.hpp>
#include <Common/Core/Profiler.hpp>
#include <Common/Core/Units.hpp>

#include <variant>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <unordered_set>

namespace Desert::Graphic::System
{
    namespace MeshRendererDetail
    {
        // Both defined in MeshRenderer.cpp; the comments on the definitions say what they answer.
        PBRGpuMaterial    BuildEffectiveMaterial( MaterialPBR* material, MaterialInstance* instance );
        MaterialInstance* FirstPBRSlot( const std::vector<MaterialInstance*>& slots, MeshVertexPath path );

        // The PassBindings of one mesh node (UE: the mesh pass's pass parameters), two blocks over the node's
        // PassContext: Plain binds nothing (the material fills every slot), Lit binds the cloud-shadow map
        // (CloudShadowMapOrWhite, declared by the node as a SampledGraphics read) as u_CloudShadowMap. For()
        // picks by what the material's SHADER declares, so a custom shader without the receiver is not handed a
        // name it does not have. A node that declares no cloud read passes an invalid ref: every draw is Plain.
        class MeshPassBindings
        {
        public:
            MeshPassBindings( const RDG::PassContext& context, RDG::TextureRef cloudShadowMap )
                 : m_Plain( context ), m_Lit( context ), m_HasCloud( cloudShadowMap.IsValid() )
            {
                if ( m_HasCloud )
                    m_Lit.Sampled( "u_CloudShadowMap", cloudShadowMap, RDG::Access::SampledGraphics,
                                   RDG::SubresourceRange::All(), RDG::SamplerDesc::LinearRepeat() );
            }

            [[nodiscard]] const RDG::PassBindings& For( const MaterialExecutor& material ) const
            {
                static const std::string kCloudShadowMap = "u_CloudShadowMap";
                return m_HasCloud && material.GetTexture2DProperty( kCloudShadowMap ) ? m_Lit : m_Plain;
            }

        private:
            RDG::PassBindings m_Plain;
            RDG::PassBindings m_Lit;
            bool              m_HasCloud = false;
        };

        // THE mesh draw of every MeshRenderer pass: Renderer::RenderMesh( bindings, ... ) with the bindings
        // For() the material. A missing pipeline, mesh or material is the pass's error, never a skipped draw.
        [[nodiscard]] inline Common::BoolResultStr
        DrawMesh( const MeshPassBindings& pass, const GraphicsPipeline* pipeline, const Mesh* mesh,
                  const glm::mat4& transform, const MaterialExecutor* material, uint32_t instanceCount = 1,
                  uint32_t firstInstance = 0, uint64_t hiddenSubmeshMask = 0, uint32_t lodLevel = 0 )
        {
            if ( pipeline == nullptr || mesh == nullptr || material == nullptr )
                return Common::MakeFormattedError( "mesh draw refused: no {}", pipeline == nullptr ? "pipeline"
                                                                               : mesh == nullptr   ? "mesh"
                                                                                                   : "material" );
            return Renderer::GetInstance().RenderMesh( pass.For( *material ), *pipeline, *mesh, transform,
                                                       *material, instanceCount, firstInstance, hiddenSubmeshMask,
                                                       lodLevel );
        }
    } // namespace MeshRendererDetail

    using MeshRendererDetail::DrawMesh;
    using MeshRendererDetail::MeshPassBindings;

    using MeshRendererDetail::BuildEffectiveMaterial;
    using MeshRendererDetail::FirstPBRSlot;
} // namespace Desert::Graphic::System
