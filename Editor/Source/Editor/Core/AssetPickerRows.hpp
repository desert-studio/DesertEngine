#pragma once

#include <Engine/Assets/ContentRegistry.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>
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

    // THE CONTENT KIND AN Asset<T> SLOT NAMES (UE: the class of a TSoftObjectPtr<T>, which is what lets its path
    // text resolve). DERIVED from the content census (Common/Content/ContentKinds.hpp), never listed: a reflected
    // asset type is `<KindName>Asset`, or the kind's own name where that already ends in Asset (PhysicsAsset). So
    // every kind the census knows - meshes, materials, textures, skyboxes, rigs, sounds, ... - is nameable by its
    // `assets:` path the moment it has a row there, and a type with no kind (the service types Font, Icon,
    // Video, which are not registry content) is nullopt and refused by name. A stated-only kind (Redirector) is
    // never the target of a reference.
    [[nodiscard]] inline std::optional<Common::Content::ContentKind>
    ContentKindForAssetType( std::string_view assetType )
    {
        constexpr std::string_view kSuffix = "Asset";
        for ( std::size_t i = 0; i < Common::Content::CONTENT_KIND_COUNT; ++i )
        {
            const auto                              kind = static_cast<Common::Content::ContentKind>( i );
            const Common::Content::ContentKindSpec& spec = Common::Content::KindSpec( kind );
            if ( spec.StatedOnly() )
                continue;
            if ( assetType == spec.Name ||
                 ( assetType.size() == spec.Name.size() + kSuffix.size() && assetType.starts_with( spec.Name ) &&
                   assetType.ends_with( kSuffix ) ) )
                return kind;
        }
        return std::nullopt;
    }

    // THE DETAILS PICKER'S LABELS for the Asset<T> slots drawn as a plain list of one kind's registry rows: the
    // picker lists the rows of that kind and accepts a dropped file only when it is one. Only the LABELS live
    // here; which kind a type names is ContentKindForAssetType's answer (the picker asks it; no row restates it),
    // and the control channel's `set` resolves an `assets:` path through that, for every kind, with the same
    // ContentRegistry::RowAtPath. A type with a richer slot (fonts, themes, rigs, clouds, textures, meshes,
    // materials) keeps its own widget in the Details panel; a static mesh is a plain row here (SLICE-01c).
    struct RegistryPickerSpec
    {
        std::string_view AssetType;
        const char*      Id;
        const char*      NoneLabel;
        const char*      Noun;
    };

    [[nodiscard]] inline const RegistryPickerSpec* RegistryPickerForAssetType( std::string_view assetType )
    {
        static constexpr RegistryPickerSpec SPECS[] = {
             { "VFXSystemAsset", "##vfxsystem", "None (plays nothing)", "VFX system" },
             { "WorldParametersAsset", "##worldparams", "None (no world parameters)",
               "world parameter collection" },
             { "PrefabAsset", "##prefab", "None (no player pawn)", "prefab" },
             { "FoliageTypeAsset", "##foliagetype", "None", "foliage type" },
             { "FractureAsset", "##fracture", "None", "fracture" },
             { "WaterWavesAsset", "##waterwaves", "None", "water waves asset" },
             { "InputMappingContextAsset", "##inputmapping", "None", "input mapping context" },
             { "PhysicsAsset", "##physicsasset", "None", "physics asset" },
             { "WeatherPresetAsset", "##weatherpreset", "None", "weather preset" },
             { "StateTreeAsset", "##statetree", "None (runs no tree)", "state tree" },
             // UE UStaticMeshComponent::StaticMesh: the reflected slot lists the registry's .stmesh rows; the
             // mesh's picture is the Preview row the component's Details draws right above it.
             { "StaticMeshAsset", "##staticmesh", "None (draws nothing)", "static mesh" },
        };
        for ( const RegistryPickerSpec& spec : SPECS )
            if ( spec.AssetType == assetType )
                return &spec;
        return nullptr;
    }
} // namespace Desert::Editor
