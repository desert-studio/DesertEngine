#pragma once

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"

#include <Common/Core/ResultStr.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <string>

namespace Desert::Geometry
{
    // UV OPERATIONS - UE's UVs palette of Modeling Mode: AutoUV (UParameterizeMeshTool, PatchBuilder method),
    // Project (UUVProjectionTool), Unwrap (URecomputeUVsTool) and Layout (UUVLayoutTool).
    //
    // Same contract as the other mesh operation headers (DynamicMeshXformOperations.hpp): pure functions, the
    // input is not touched, the result is a NEW mesh whose UV layer 0 is rewritten (a mesh without one gets it)
    // and whose tangents, derived from those UVs, are rebuilt. A refusal names what and why. Units are
    // centimetres; +Y is up.

    struct UVOutcome
    {
        DynamicMesh3 Mesh;
        int32_t      Islands = 0; // UV islands in the result
        std::string  Report;      // what the tool did, for the log
    };

    // ---- AutoUV (UE UParameterizeMeshTool, Method = PatchBuilder) ----
    // UE FPatchBasedMeshUVGenerator: patches of triangles whose normals stay within MaxNormalDeviationDeg of
    // their patch's normal (UE grows initial patches by area-density point sampling, then merges them through
    // FMeshRegionGraph; here a patch grows from its seed directly under the same deviation bound), each patch
    // flattened by a discrete exponential map, oriented to its smallest bounding box and packed.
    struct AutoUVSettings
    {
        float   MaxNormalDeviationDeg = 45.0f; // UE MaxAngleDeviationDeg
        bool    RespectPolygroups     = false; // UE bRespectInputGroups: a patch never crosses a polygroup border
        bool    AutoAlign             = true;  // UE bAutoAlignPatches
        bool    AutoPack              = true;  // UE bAutoPack
        int32_t PackingResolution     = 512;   // UE PackingTextureResolution

        bool operator==( const AutoUVSettings& ) const = default;
    };
    [[nodiscard]] Common::ResultStr<UVOutcome> AutoUV( const DynamicMesh3& mesh, const AutoUVSettings& settings );

    // ---- Project (UE UUVProjectionTool) ----
    enum class UVProjectionType : uint8_t
    {
        Plane,
        Box,
        Cylinder,
    };
    [[nodiscard]] const char* ToString( UVProjectionType type );

    // The projection frame is the mesh's bounds centre plus CenterOffset, turned by RotationDeg (XYZ Euler,
    // degrees); its Z axis is the mesh's +Y (up), so Plane projects from above and Cylinder stands upright.
    // UV = position in the frame / Dimensions, then the UV transform (UE UVRotation / UVScale / UVTranslate).
    struct UVProjectSettings
    {
        UVProjectionType Type        = UVProjectionType::Box;
        bool             FitToBounds = true;   // UE Initialization = AutoFit: Dimensions = bounds size
        glm::vec3        Dimensions{ 100.0f }; // cm, used when FitToBounds is off (UE default 100)
        glm::vec3        CenterOffset{ 0.0f }; // cm, from the bounds centre
        glm::vec3        RotationDeg{ 0.0f };
        float            CylinderSplitAngle = 45.0f; // degrees: within it of the axis a face takes a cap
        float            UVRotationDeg      = 0.0f;
        glm::vec2        UVScale{ 1.0f };
        glm::vec2        UVTranslate{ 0.0f };

        bool operator==( const UVProjectSettings& ) const = default;
    };
    [[nodiscard]] Common::ResultStr<UVOutcome> ProjectUVs( const DynamicMesh3&      mesh,
                                                           const UVProjectSettings& settings );

    // ---- Unwrap (UE URecomputeUVsTool) ----
    enum class UVIslandSource : uint8_t
    {
        PolyGroups,  // one island per polygroup (connected part of it)
        ExistingUVs, // the mesh's current UV islands
    };
    enum class UVUnwrapMethod : uint8_t
    {
        ExpMap,            // UE ERecomputeUVsUnwrapType::ExpMap
        SpectralConformal, // UE ERecomputeUVsUnwrapType::SpectralConformal
    };
    [[nodiscard]] const char* ToString( UVIslandSource source );
    [[nodiscard]] const char* ToString( UVUnwrapMethod method );

    // An island with no boundary (a closed polygroup) cannot be flattened by either method: it is projected
    // onto the plane of its average normal instead, and the Report counts it.
    struct UVUnwrapSettings
    {
        UVIslandSource Source            = UVIslandSource::PolyGroups;
        UVUnwrapMethod Method            = UVUnwrapMethod::SpectralConformal;
        bool           AutoRotate        = true; // UE AutoRotation = MinBounds
        bool           NormalizeAreas    = true; // UE bNormalizeAreas: UV area matches world area
        bool           Pack              = true; // UE LayoutType = Repack
        int32_t        PackingResolution = 1024;

        bool operator==( const UVUnwrapSettings& ) const = default;
    };
    [[nodiscard]] Common::ResultStr<UVOutcome> UnwrapUVs( const DynamicMesh3&     mesh,
                                                          const UVUnwrapSettings& settings );

    // ---- Layout (UE UUVLayoutTool) ----
    enum class UVLayoutType : uint8_t
    {
        Repack,    // UE EUVLayoutType::Repack: every island packed into the unit square
        Transform, // UE EUVLayoutType::Transform: only the scale and translation below
    };
    [[nodiscard]] const char* ToString( UVLayoutType type );

    struct UVLayoutSettings
    {
        UVLayoutType Type              = UVLayoutType::Repack;
        int32_t      TextureResolution = 1024;
        bool         PreserveScale     = false;
        bool         PreserveRotation  = false;
        bool         AllowFlips        = false;
        float        Scale             = 1.0f; // UE UVScaleFactor, after packing
        glm::vec2    Translation{ 0.0f };      // UE UVTranslation, after packing

        bool operator==( const UVLayoutSettings& ) const = default;
    };
    // Refused for a mesh with no UV layer 0 or no UV-set triangle.
    [[nodiscard]] Common::ResultStr<UVOutcome> LayoutUVs( const DynamicMesh3&     mesh,
                                                          const UVLayoutSettings& settings );
} // namespace Desert::Geometry
