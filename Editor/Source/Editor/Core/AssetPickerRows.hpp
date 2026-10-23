#pragma once

#include <Engine/Assets/ContentRegistry.hpp>

#include <filesystem>
#include <string>
#include <string_view>

namespace Desert::Editor
{
    // WHAT A PICKER SHOWS FOR ONE REGISTRY ROW: the display name the file states (the registry's Name tag,
    // read by the scan from the document's top level, never by loading the asset), else the file's stem —
    // exactly what those assets fall back to themselves when the file states none.
    [[nodiscard]] inline std::string PickerDisplayName( const Assets::ContentRegistry::PickerRow& row )
    {
        return row.DisplayName.empty() ? row.Path.stem().string() : row.DisplayName;
    }

    // THE REFLECTED ASSET TYPES (PROPERTY Asset<T>) WHOSE TARGET IS A CONTENT-REGISTRY ROW OF ONE KIND (UE
    // TSoftObjectPtr<T>): the Details picker lists the rows of that kind and accepts a dropped file only when it
    // is one, and the control channel's `set` resolves an `assets:` path through the same table and the same
    // ContentRegistry::RowAtPath - one resolver for both, so the two cannot accept different things. A type with a
    // richer slot (fonts, themes, rigs, clouds, textures) keeps its own branch in the Details panel.
    struct RegistryPickerSpec
    {
        std::string_view             AssetType;
        Common::Content::ContentKind Kind;
        const char*                  Id;
        const char*                  NoneLabel;
        const char*                  Noun;
    };

    [[nodiscard]] inline const RegistryPickerSpec* RegistryPickerForAssetType( std::string_view assetType )
    {
        using K                                     = Common::Content::ContentKind;
        static constexpr RegistryPickerSpec SPECS[] = {
             { "VFXSystemAsset", K::VFXSystem, "##vfxsystem", "None (plays nothing)", "VFX system" },
             { "WorldParametersAsset", K::WorldParameters, "##worldparams", "None (no world parameters)",
               "world parameter collection" },
             { "PrefabAsset", K::Prefab, "##prefab", "None (no player pawn)", "prefab" },
             { "FoliageTypeAsset", K::FoliageType, "##foliagetype", "None", "foliage type" },
             { "FractureAsset", K::Fracture, "##fracture", "None", "fracture" },
             { "WaterWavesAsset", K::WaterWaves, "##waterwaves", "None", "water waves asset" },
             { "InputMappingContextAsset", K::InputMappingContext, "##inputmapping", "None",
               "input mapping context" },
             { "PhysicsAsset", K::PhysicsAsset, "##physicsasset", "None", "physics asset" },
             { "WeatherPresetAsset", K::WeatherPreset, "##weatherpreset", "None", "weather preset" },
             { "StateTreeAsset", K::StateTree, "##statetree", "None (runs no tree)", "state tree" },
        };
        for ( const RegistryPickerSpec& spec : SPECS )
            if ( spec.AssetType == assetType )
                return &spec;
        return nullptr;
    }
} // namespace Desert::Editor
