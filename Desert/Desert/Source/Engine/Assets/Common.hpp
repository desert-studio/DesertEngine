#pragma once

#include <Common/Core/AssetHandle.hpp>

namespace Desert::Assets
{
    using AssetHandle = Common::AssetHandle;

    template <typename T>
    using Asset = std::shared_ptr<T>;

    using NullAsset = nullptr_t;

    enum class AssetPriority
    {
        Low    = 0,
        Medium = 1,
        High   = 2,
    };

    enum class AssetTypeID
    {
        Unknown = 0,
        Mesh,
        Material,
        Texture2D,
        Skybox,
        Shader,
        Skeleton,
        Animation,
        Prefab,
        // The volumetric clouds' 3D noise (`.dcnv`). A first-class asset rather than a bake output, so an
        // artist can author several and drop one into the cloud component's slot — see
        // Engine/Assets/CloudNoiseVolume.hpp.
        CloudNoiseVolume,
        // A named kind of cloud (`.decloudtype`): the twelve numbers a vertical profile is generated from,
        // plus the noise volume its edge is cut from. A first-class asset because the owner's request was
        // exactly that an artist be able to make one and load it into a slot — see
        // Engine/Assets/CloudTypeData.hpp.
        CloudType,
        // A sculpted cloud BODY (`.dcmv`): 128 x 64 x 128 voxels of dimensional profile, detail type,
        // density scale and cutout envelope, placed in the sky by an entity's transform. The seam's
        // authored producer — the half of the cloud field that can be a shape the procedural one cannot
        // make — see Engine/Assets/CloudModellingVolume.hpp.
        CloudModellingVolume,
        // A PAINTED cloud layout (`.dclayout`): a four-channel pattern saying where each of the layer's
        // species slots lives, and a signed mask that adds or removes cloud regionally. Unreal's
        // `Layout_CloudGlobalPattern` and `Layout_GlobalCloudMask` as data rather than as a material graph
        // — decision D-5 stands, and these are tables, not nodes. Read at the BAKE and never in the march,
        // so a painting costs the hottest pass of the frame nothing — see Engine/Assets/CloudLayout.hpp.
        CloudLayout,
        // A UI THEME (`.detheme`): named colours, metrics and fonts, plus the styles that bind them to
        // the UI element set's slots. A first-class asset because a theme that is not a file cannot be
        // made, named, duplicated or dropped into a canvas's slot, and a game that ships two looks would
        // otherwise need the engine rebuilt to get the second one — see Engine/Assets/UIThemeData.hpp.
        UITheme,

        // NOT an asset type: the number of them. Every new type is added ABOVE this line, and adding one
        // turns the AssetHandleStability census red until the type is entered in that suite's catalogue.
        // That red is the only reason this enumerator exists: an asset type whose handle stability nobody
        // asserts is exactly how five types kept a random per-launch identity for as long as they did.
        Count,
    };

    // The enumerator's name, for the one caller that has to say WHICH types disagreed: AssetManager's
    // typed lookup, when a record turns out to hold something other than what was asked for. "type 4 was
    // requested as type 10" is a number the reader then has to go and decode; "Skybox was requested as
    // CloudType" is the defect itself, spelled out.
    //
    // Deliberately a switch with NO `default:` label: -Wswitch then reports a new enumerator here, at the
    // point of the omission. A `default:` would swallow it and put the wrong name in the one message whose
    // whole job is to be trusted — and a name table that quietly falls behind its enum is exactly the
    // two-places-must-agree defect this subsystem keeps paying for. The trailing return is NOT that
    // default: it is reached only by a value no enumerator names (an enum's underlying type admits those),
    // and every named one is handled above. AssetHandleStability asserts that claim over the whole enum.
    constexpr const char* AssetTypeName( const AssetTypeID type )
    {
        switch ( type )
        {
            case AssetTypeID::Unknown:
                return "Unknown";
            case AssetTypeID::Mesh:
                return "Mesh";
            case AssetTypeID::Material:
                return "Material";
            case AssetTypeID::Texture2D:
                return "Texture2D";
            case AssetTypeID::Skybox:
                return "Skybox";
            case AssetTypeID::Shader:
                return "Shader";
            case AssetTypeID::Skeleton:
                return "Skeleton";
            case AssetTypeID::Animation:
                return "Animation";
            case AssetTypeID::Prefab:
                return "Prefab";
            case AssetTypeID::CloudNoiseVolume:
                return "CloudNoiseVolume";
            case AssetTypeID::CloudType:
                return "CloudType";
            case AssetTypeID::CloudModellingVolume:
                return "CloudModellingVolume";
            case AssetTypeID::CloudLayout:
                return "CloudLayout";
            case AssetTypeID::UITheme:
                return "UITheme";
            case AssetTypeID::Count:
                return "Count";
        }
        return "Unknown";
    }
} // namespace Desert::Assets