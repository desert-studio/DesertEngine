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
        // the parent again on the promise that FirstSurfaceSlot already checked it.
        struct SurfaceSlot
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
        SurfaceSlot  FirstSurfaceSlot( const std::vector<MaterialInstance*>& slots, MeshVertexPath path );
        std::optional<std::string>          DefaultSurfaceShaderName( MeshVertexPath path, MeshPass pass );
        std::shared_ptr<Shader>             DefaultSurfaceProgram( MeshVertexPath path, MeshPass pass );
        std::shared_ptr<Shader>             DefaultSurfaceProgramVariant( MeshVertexPath path, MeshPass pass,
                                                                          const ShaderVariant& variant );
        std::shared_ptr<DataDrivenMaterial> CreateCellMaterial( MeshVertexPath path,
                                                                MeshPass       pass = MeshPass::Forward );
        std::string                         MeshPathOwnBufferName( MeshVertexPath path );

        static_assert( SceneResources::kMaxCascades == kSceneViewShadowCascades,
                       "the lit materials' cascade count is the scene/view inputs' cascade count" );

    } // namespace MeshRendererDetail

    using MeshRendererDetail::MeshDrawList;

    using MeshRendererDetail::AppendRow;
    using MeshRendererDetail::CreateCellMaterial;
    using MeshRendererDetail::DefaultSurfaceProgram;
    using MeshRendererDetail::DefaultSurfaceProgramVariant;
    using MeshRendererDetail::DefaultSurfaceShaderName;
    using MeshRendererDetail::EffectiveRow;
    using MeshRendererDetail::FirstSurfaceSlot;
    using MeshRendererDetail::IsTranslucent;
    using MeshRendererDetail::MeshPathOwnBufferName;
    using MeshRendererDetail::SurfaceSlot;
} // namespace Desert::Graphic::System
