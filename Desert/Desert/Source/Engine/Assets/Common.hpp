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
        // A table of translated strings (`.destrings`): key -> text, per language, per grammatical form.
        // A first-class asset because it is authored content with a file of its own, and because loading
        // one PUBLISHES it to the process's lookup — which is what makes a translation edit reach the
        // screen without a restart. See Engine/Localization/StringTable.hpp.
        StringTable,

        // A CONTROL RIG (`.derig`): the controls an animator grabs, their parent spaces, and which bone
        // each one drives. A first-class asset because tier T5 shipped four working halves of a rig — a
        // hierarchy, a manipulator, keying and a pipeline stage — and NO way for a scene to have one: a
        // `ControlRigStage` had to be built in C++ by a caller that did not exist. See
        // Engine/Assets/Serialization/ControlRig.hpp.
        ControlRig,

        // NOT an asset type: the number of them. Every new type is added ABOVE this line, and adding one
        // turns the AssetHandleStability census red until the type is entered in that suite's catalogue.
        // That red is the only reason this enumerator exists: an asset type whose handle stability nobody
        // asserts is exactly how five types kept a random per-launch identity for as long as they did.
        Count,
    };

    /**
     * @brief Is this type's lifetime the PROJECT's rather than the scene's?
     *
     * MEASURED, not assumed. The eviction sweep traces reachability from the components of every live
     * scene (Engine/Core/SceneAssetRoots.hpp), so an asset no component NAMES is unreachable by
     * construction and is released. A string table is named by no component and can never be: a `#key` in
     * an authored label is a STRING, not an `AssetHandle`, which is the whole point of the sigil. So the
     * first sweep after a scene loaded swept both shipped tables out — measured on UI_ElementProbe, where
     * the log read "String table ... loaded: 22 keys" at boot and "key 'probe.title' is in none of the 0
     * loaded string tables" three seconds later, with every keyed label on screen replaced by its own key.
     *
     * The fix is not a root: there is nothing to hang one on. It is the statement that some content does
     * not belong to a world at all. A `switch` with NO `default:` so that -Wswitch reports a new asset
     * type here, at the point where somebody has to decide which side of the line it is on.
     */
    constexpr bool IsProjectScopedAsset( const AssetTypeID type )
    {
        switch ( type )
        {
            case AssetTypeID::StringTable:
                return true;
            case AssetTypeID::Unknown:
            case AssetTypeID::Mesh:
            case AssetTypeID::Material:
            case AssetTypeID::Texture2D:
            case AssetTypeID::Skybox:
            case AssetTypeID::Shader:
            case AssetTypeID::Skeleton:
            case AssetTypeID::Animation:
            case AssetTypeID::Prefab:
            case AssetTypeID::CloudNoiseVolume:
            case AssetTypeID::CloudType:
            case AssetTypeID::CloudModellingVolume:
            case AssetTypeID::CloudLayout:
            // A THEME IS SCENE-SCOPED, and that is a decision rather than the fallthrough it used to be.
            // It looks like a sibling of StringTable — both are project-wide authoring concepts — but the
            // property this switch asks about is not "how does an author think of it", it is "can the
            // reachability walk SEE it". A string table cannot be seen: a `#key` in a label is a string,
            // which is the whole point of the sigil. A theme can: `UICanvasData::Theme` is an
            // `AssetHandle`, and SceneAssetRoots.cpp:101 marks it with "a UI canvas is themed by it". So
            // the ordinary rule already holds it alive for exactly as long as a live canvas names it, and
            // exempting it from eviction would keep every theme ever opened resident for nothing.
            case AssetTypeID::UITheme:
            // A RIG IS SCENE-SCOPED for the theme's reason and by the same mechanism:
            // `ControlRigData::Rig` is an `AssetHandle`, so the reachability walk can see it and the
            // ordinary rule holds it alive for exactly as long as a live entity names it.
            case AssetTypeID::ControlRig:
            case AssetTypeID::Count:
                return false;
        }
        // Unreachable for every enumerator above.
        return false;
    }

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
            case AssetTypeID::StringTable:
                return "StringTable";
            case AssetTypeID::ControlRig:
                return "ControlRig";
            case AssetTypeID::Count:
                return "Count";
        }
        return "Unknown";
    }
} // namespace Desert::Assets