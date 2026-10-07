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
#include <Engine/Graphic/Materials/DataDrivenMaterial.hpp>
#include <Engine/Graphic/Materials/SceneResources.hpp>
#include <Engine/Graphic/ShadowCascades.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Core/Formats/MaterialParamRow.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>
#include <Engine/Geometry/LODSelection.hpp>
#include <Engine/Geometry/MeshBounds.hpp>
#include <Engine/Graphic/VisibilityCulling.hpp>
#include <Engine/Runtime/Services/Material/MaterialService.hpp>
// MeshShaderFor / MeshVertexPath / MeshPass — the (path x pass) table this file asks for its pipelines.
#include <Engine/Graphic/Materials/Mesh/MeshVertexPath.hpp>
#include <Engine/Graphic/Systems/Scene/Mesh/TranslucentSortOrder.hpp>
#include <Engine/Graphic/Materials/Mesh/MeshVertexLayout.hpp>
#include <Engine/Graphic/Materials/Mesh/InstancedRecorder.hpp>
#include <Engine/Graphic/ShaderProtocols/SkinnedMaterialUB.hpp>
#include <Common/Core/Profiler.hpp>
#include <Common/Core/Units.hpp>

#include <type_traits>
#include <variant>
#include <chrono>
#include <format>
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
        // The slot a batched path draws, AND its surface: the one cast is asked here, so no caller downcasts
        // the parent again on the promise that FirstPBRSlot already checked it.
        struct PBRSlot
        {
            MaterialInstance*   Instance = nullptr;
            DataDrivenMaterial* Surface  = nullptr;
            explicit            operator bool() const
            {
                return Instance != nullptr;
            }
        };

        // Defined in MeshRenderer.cpp; the comments on the definitions say what they answer.
        Core::Formats::MaterialParamRow EffectiveRow( const DataDrivenMaterial* material,
                                                      MaterialInstance*         instance );
        bool                            IsTranslucent( const DataDrivenMaterial* material );
        uint32_t AppendRow( std::vector<glm::vec4>& rows, const Core::Formats::MaterialParamRow& row );
        PBRSlot  FirstPBRSlot( const std::vector<MaterialInstance*>& slots, MeshVertexPath path );
        std::optional<std::string>          DefaultSurfaceShaderName( MeshVertexPath path, MeshPass pass );
        std::shared_ptr<Shader>             DefaultSurfaceProgram( MeshVertexPath path, MeshPass pass );
        std::shared_ptr<DataDrivenMaterial> CreateCellMaterial( MeshVertexPath path,
                                                                MeshPass       pass = MeshPass::Forward );
        std::string                         MeshPathOwnBufferName( MeshVertexPath path );

        static_assert( SceneResources::kMaxCascades == kSceneViewShadowCascades,
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

    using MeshRendererDetail::AppendRow;
    using MeshRendererDetail::CreateCellMaterial;
    using MeshRendererDetail::DefaultSurfaceProgram;
    using MeshRendererDetail::DefaultSurfaceShaderName;
    using MeshRendererDetail::EffectiveRow;
    using MeshRendererDetail::FirstPBRSlot;
    using MeshRendererDetail::IsTranslucent;
    using MeshRendererDetail::MeshPathOwnBufferName;
    using MeshRendererDetail::PBRSlot;
} // namespace Desert::Graphic::System
