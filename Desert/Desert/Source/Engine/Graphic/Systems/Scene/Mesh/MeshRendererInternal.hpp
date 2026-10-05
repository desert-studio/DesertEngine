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

        // The PassBindings of one mesh node (UE: the mesh pass's pass parameters) over the node's PassContext.
        // Plain binds nothing (the material fills every slot it owns); a node constructed with the frame's
        // SceneViewInputs binds them (BindSceneViewInputs: shadow cascades, environment cubes, BRDF LUT, cloud
        // shadow map) for every draw whose SHADER samples them, by its reflected resource list - so a custom
        // shader without the receivers is never handed a name it does not have. One block per shader, built on
        // its first draw in the node.
        class MeshPassBindings
        {
        public:
            explicit MeshPassBindings( const RDG::PassContext& context ) : m_Context( context ), m_Plain( context )
            {
            }
            MeshPassBindings( const RDG::PassContext& context, const SceneViewInputs& view )
                 : m_Context( context ), m_Plain( context ), m_View( view )
            {
            }

            [[nodiscard]] const RDG::PassBindings& For( const MaterialExecutor& material ) const
            {
                const std::shared_ptr<Shader> shader = material.GetShader();
                if ( !m_View || !shader )
                    return m_Plain;
                auto it = m_ByShader.find( shader.get() );
                if ( it == m_ByShader.end() )
                {
                    std::unique_ptr<RDG::PassBindings> lit;
                    if ( SamplesSceneViewInputs( *shader ) )
                    {
                        lit = std::make_unique<RDG::PassBindings>( m_Context );
                        BindSceneViewInputs( *lit, *m_View, *shader );
                    }
                    it = m_ByShader.emplace( shader.get(), std::move( lit ) ).first;
                }
                return it->second ? *it->second : m_Plain;
            }

        private:
            const RDG::PassContext&        m_Context;
            RDG::PassBindings              m_Plain;
            std::optional<SceneViewInputs> m_View;
            // nullptr: the shader samples no scene/view input (Plain).
            mutable std::unordered_map<const Shader*, std::unique_ptr<RDG::PassBindings>> m_ByShader;
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
