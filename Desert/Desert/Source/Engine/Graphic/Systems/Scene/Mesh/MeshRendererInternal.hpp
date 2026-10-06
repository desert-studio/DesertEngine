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
#include <Engine/Graphic/Materials/Mesh/PBR/MaterialPBRBase.hpp>
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
#include <unordered_map>
#include <optional>
#include <memory>

namespace Desert::Graphic::System
{
    namespace MeshRendererDetail
    {
        // Both defined in MeshRenderer.cpp; the comments on the definitions say what they answer.
        PBRGpuMaterial    BuildEffectiveMaterial( MaterialPBR* material, MaterialInstance* instance );
        MaterialInstance* FirstPBRSlot( const std::vector<MaterialInstance*>& slots, MeshVertexPath path );

        static_assert( MaterialPBRBase::kMaxCascades == kSceneViewShadowCascades,
                       "the lit materials' cascade count is the scene/view inputs' cascade count" );

        // The PassBindings of a mesh node whose shaders sample no graph resource (shadow, RSM, silhouette, overdraw):
        // every slot is the material's. The lit nodes declare their blocks in setup instead (MeshDrawList).
        class MeshPassBindings
        {
        public:
            explicit MeshPassBindings( const RDG::PassContext& context ) : m_Plain( context )
            {
            }

            [[nodiscard]] const RDG::PassBindings& For( const MaterialExecutor& /*material*/ ) const
            {
                return m_Plain;
            }

        private:
            RDG::PassBindings m_Plain;
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
    using MeshRendererDetail::MeshDrawList;
    using MeshRendererDetail::MeshPassBindings;

    using MeshRendererDetail::BuildEffectiveMaterial;
    using MeshRendererDetail::FirstPBRSlot;
} // namespace Desert::Graphic::System
