#pragma once
// HOW AN ASSET IS PHOTOGRAPHED FOR ITS THUMBNAIL, STORED WITH THE ASSET (UE: UThumbnailInfo on the package).
//
// UE keeps this on the asset, not in the editor: a mesh carries a USceneThumbnailInfo (OrbitPitch, OrbitYaw,
// OrbitZoom — SceneThumbnailInfo.h), a material a USceneThumbnailInfoWithPrimitive (the same orbit plus the
// primitive it is drawn on and an optional PreviewMesh — SceneThumbnailInfoWithPrimitive.h), and the thumbnail
// renderer reads ONLY that object. The same split here:
//
//   ThumbnailOrbit   the orbit every photographed asset has; a mesh stores it in its THMB section
//                    (MeshSourceAsset.hpp), absent = the default orbit
//   ThumbnailInfo    a material's: the primitive, its own mesh instead of the primitive, and the orbit; it lives
//                    in MaterialData::Thumbnail (.demat), absent = the default info
//
// The default of each is the framing the thumbnails had before the info existed (the subject seen straight on,
// fitted by ThumbnailFraming's rule), so an asset that never states one keeps its picture. That is the format's
// meaning of "absent", not a fallback: there is exactly one default and it is written here.
//
// Which PICTURE a material gets (the ball, a mesh, the sky dome) is still decided by its shader's DOMAIN
// (Editor ThumbnailSubject.hpp PreviewForMaterial): the primitive and the orbit only apply where the mesh path
// draws the material. A Volume material's sky is not a user choice and this type does not offer it.
#include <Engine/Assets/AssetGuidRef.hpp>

#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>

namespace Desert::Assets
{
    // The shape a material is photographed on when it names no mesh of its own (UE EThumbnailPrimType minus
    // TPT_None: "no primitive" is said here by naming a PreviewMesh).
    enum class ThumbnailPrimitive
    {
        Sphere,
        Cube,
        Plane,
        Cylinder,
    };

    // The camera's orbit around the subject, in degrees (UE USceneThumbnailInfo). Pitch turns the view over the
    // subject's right axis (positive = looking from above), Yaw about the world up axis; Zoom moves the camera
    // along its view axis as a fraction of the fitted distance (0 = the framing rule's fit, 0.5 = half as far
    // again, -0.5 = half-way in). Zoom must stay above -1: at -1 the camera would sit in the subject.
    struct ThumbnailOrbit
    {
        float Pitch = 0.0f;
        float Yaw   = 0.0f;
        float Zoom  = 0.0f;

        bool operator==( const ThumbnailOrbit& ) const = default;
    };

    [[nodiscard]] inline bool IsValidThumbnailOrbit( const ThumbnailOrbit& orbit )
    {
        return std::isfinite( orbit.Pitch ) && std::isfinite( orbit.Yaw ) && std::isfinite( orbit.Zoom ) &&
               orbit.Zoom > -1.0f;
    }

    // A material's thumbnail info (UE USceneThumbnailInfoWithPrimitive).
    struct ThumbnailInfo
    {
        ThumbnailPrimitive Primitive = ThumbnailPrimitive::Sphere;

        // THE MESH THIS MATERIAL IS PHOTOGRAPHED ON INSTEAD OF THE PRIMITIVE, by the mesh's header GUID (its
        // `.deimport` or `.stmesh` header); `Path` is the mesh SOURCE relative to the assets root, a locator only.
        // A grass atlas previews as the tuft it was authored for rather than cut out of a ball. It is a stated
        // dependency of the material (MaterialData::DependencyGuids), so the header names the edge.
        std::optional<AssetGuidRef> PreviewMesh;

        ThumbnailOrbit Orbit;

        bool operator==( const ThumbnailInfo& ) const = default;
    };

    [[nodiscard]] constexpr std::string_view ThumbnailPrimitiveName( ThumbnailPrimitive primitive )
    {
        switch ( primitive )
        {
            case ThumbnailPrimitive::Sphere:
                return "Sphere";
            case ThumbnailPrimitive::Cube:
                return "Cube";
            case ThumbnailPrimitive::Plane:
                return "Plane";
            case ThumbnailPrimitive::Cylinder:
                return "Cylinder";
        }
        return "?";
    }
} // namespace Desert::Assets
