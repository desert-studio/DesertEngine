#pragma once
// HOW AN ASSET IS PHOTOGRAPHED FOR ITS THUMBNAIL, STORED WITH THE ASSET (UE: UThumbnailInfo on the package).
//
// UE keeps this on the asset, not in the editor: a mesh carries a USceneThumbnailInfo (OrbitPitch, OrbitYaw,
// OrbitZoom — SceneThumbnailInfo.h), a material a USceneThumbnailInfoWithPrimitive (the same orbit plus the
// primitive it is drawn on and an optional PreviewMesh — SceneThumbnailInfoWithPrimitive.h), and the thumbnail
// renderer reads ONLY that object. The same split here:
//
//   ThumbnailOrbit   the orbit every photographed asset has; an imported mesh's lives in its package, the
//                    import record (ImportRecordData::Thumbnail, one entry per mesh the import writes), no
//                    entry = the default orbit
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

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>

namespace Desert::Assets
{
    // The shape a material is photographed on when it names no mesh of its own (UE EThumbnailPrimType minus
    // TPT_None: "no primitive" is said here by naming a PreviewMesh).
    enum class ThumbnailPrimitive : int
    {
        Sphere,
        Cube,
        Plane,
        Cylinder,
    };

    // Every primitive, in the order the editor offers them.
    inline constexpr std::array<ThumbnailPrimitive, 4> kThumbnailPrimitives = {
         ThumbnailPrimitive::Sphere, ThumbnailPrimitive::Cube, ThumbnailPrimitive::Plane,
         ThumbnailPrimitive::Cylinder };

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

    // THE STORED FORM OF BOTH (UE writes a ThumbnailInfo's properties only where they differ from the class
    // default, and a package without one reads as the defaults). The records are what the file carries: every
    // member may be absent, and an absent member IS the default above — a hand-written `"Thumbnail":
    // {"Primitive":"Cube"}` is a complete statement, not a damaged one. The project's JSON door is strict
    // (Common/Json/Json.hpp: a missing member is an error unless its type says it may be absent), so the
    // optionality is spelled here, in the one pair of types that is read and written, and the rest of the
    // engine keeps the resolved ThumbnailOrbit / ThumbnailInfo. ToRecord writes a member only when it differs
    // from its default, so one picture has one spelling.
    struct ThumbnailOrbitRecord
    {
        std::optional<float> Pitch;
        std::optional<float> Yaw;
        std::optional<float> Zoom;

        bool operator==( const ThumbnailOrbitRecord& ) const = default;
    };

    struct ThumbnailInfoRecord
    {
        std::optional<ThumbnailPrimitive>   Primitive;
        std::optional<AssetGuidRef>         PreviewMesh;
        std::optional<ThumbnailOrbitRecord> Orbit;

        bool operator==( const ThumbnailInfoRecord& ) const = default;
    };

    [[nodiscard]] inline ThumbnailOrbit Resolve( const ThumbnailOrbitRecord& record )
    {
        const ThumbnailOrbit fallback{};
        return ThumbnailOrbit{ record.Pitch.value_or( fallback.Pitch ), record.Yaw.value_or( fallback.Yaw ),
                               record.Zoom.value_or( fallback.Zoom ) };
    }

    [[nodiscard]] inline ThumbnailInfo Resolve( const ThumbnailInfoRecord& record )
    {
        ThumbnailInfo info;
        if ( record.Primitive.has_value() )
            info.Primitive = *record.Primitive;
        info.PreviewMesh = record.PreviewMesh;
        if ( record.Orbit.has_value() )
            info.Orbit = Resolve( *record.Orbit );
        return info;
    }

    [[nodiscard]] inline ThumbnailOrbitRecord ToRecord( const ThumbnailOrbit& orbit )
    {
        const ThumbnailOrbit fallback{};
        ThumbnailOrbitRecord record;
        if ( orbit.Pitch != fallback.Pitch )
            record.Pitch = orbit.Pitch;
        if ( orbit.Yaw != fallback.Yaw )
            record.Yaw = orbit.Yaw;
        if ( orbit.Zoom != fallback.Zoom )
            record.Zoom = orbit.Zoom;
        return record;
    }

    [[nodiscard]] inline ThumbnailInfoRecord ToRecord( const ThumbnailInfo& info )
    {
        const ThumbnailInfo fallback{};
        ThumbnailInfoRecord record;
        if ( info.Primitive != fallback.Primitive )
            record.Primitive = info.Primitive;
        record.PreviewMesh = info.PreviewMesh;
        if ( info.Orbit != fallback.Orbit )
            record.Orbit = ToRecord( info.Orbit );
        return record;
    }

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
