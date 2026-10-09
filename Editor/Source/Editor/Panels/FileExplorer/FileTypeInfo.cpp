#include "FileTypeInfo.hpp"

#include <Editor/Core/IconsMaterialDesignIcons.hpp>

#include <array>
#include <cstddef>

namespace Desert::Editor
{
    namespace
    {
        // A kind with no class colour of its own draws white; one with no glyph of its own draws the generic file.
        constexpr ImVec4 kNoColour{ 1.0f, 1.0f, 1.0f, 1.0f };

        // IN ENUM ORDER, one row per kind: FileTypeInfoOf indexes it, and the static_assert below refuses a
        // table that skips, repeats or reorders a kind.
        constexpr std::array kRows{
             FileTypeInfo{ FileType::Unknown, "Unknown", kNoColour, ICON_MDI_FILE },
             FileTypeInfo{ FileType::Scene, "Scene", { 0.8f, 0.4f, 0.22f, 1.00f }, ICON_MDI_FILE },
             FileTypeInfo{ FileType::Prefab, "Prefab", { 0.10f, 0.50f, 0.80f, 1.00f }, ICON_MDI_FILE },
             FileTypeInfo{ FileType::Script, "Script", { 0.10f, 0.50f, 0.80f, 1.00f }, ICON_MDI_LANGUAGE_LUA },
             FileTypeInfo{ FileType::Audio, "Audio", { 0.20f, 0.80f, 0.50f, 1.00f }, ICON_MDI_MICROPHONE },
             FileTypeInfo{
                  FileType::Shader, "Shader", { 0.10f, 0.50f, 0.80f, 1.00f }, ICON_MDI_IMAGE_FILTER_BLACK_WHITE },
             FileTypeInfo{ FileType::Texture, "Texture", { 0.82f, 0.20f, 0.33f, 1.00f }, ICON_MDI_FILE_IMAGE },
             FileTypeInfo{
                  FileType::Cubemap, "Cubemap", { 0.82f, 0.18f, 0.30f, 1.00f }, ICON_MDI_IMAGE_FILTER_HDR },
             FileTypeInfo{ FileType::Model, "Model", { 0.18f, 0.82f, 0.76f, 1.00f }, ICON_MDI_VECTOR_POLYGON },
             FileTypeInfo{ FileType::Material, "Material", kNoColour, ICON_MDI_FILE },
             FileTypeInfo{ FileType::ShaderGraph, "Shader Graph", { 0.55f, 0.35f, 0.85f, 1.00f }, ICON_MDI_GRAPH },
             FileTypeInfo{ FileType::Ini, "Settings", { 0.65f, 0.65f, 0.68f, 1.00f }, ICON_MDI_FILE_DOCUMENT },
             FileTypeInfo{ FileType::Font, "Font", { 0.60f, 0.19f, 0.32f, 1.00f }, ICON_MDI_FORMAT_FONT },
             // The same glyph the cloud-type document registers itself with (the subject-editor registration),
             // so the browser tile and the window it opens are recognisably the same thing.
             FileTypeInfo{ FileType::Cloud, "Cloud", { 0.62f, 0.78f, 0.95f, 1.00f }, ICON_MDI_WEATHER_CLOUDY },
             FileTypeInfo{ FileType::UITheme, "UI Theme", { 0.95f, 0.72f, 0.30f, 1.00f }, ICON_MDI_PALETTE },
             FileTypeInfo{ FileType::LandscapeLayerInfo,
                           "Landscape Layer Info",
                           { 0.45f, 0.70f, 0.30f, 1.00f },
                           ICON_MDI_LAYERS },
             FileTypeInfo{
                  FileType::LevelSequence, "Level Sequence", { 0.85f, 0.35f, 0.25f, 1.00f }, ICON_MDI_MOVIE_OPEN },
             FileTypeInfo{ FileType::VFXSystem, "VFX System", { 0.95f, 0.45f, 0.10f, 1.00f }, ICON_MDI_FIRE },
             FileTypeInfo{
                  FileType::Fracture, "Fracture", { 0.75f, 0.55f, 0.35f, 1.00f }, ICON_MDI_CUBE_UNFOLDED },
             FileTypeInfo{ FileType::ImportSettings,
                           "Import Settings",
                           { 0.65f, 0.65f, 0.68f, 1.00f },
                           ICON_MDI_FILE_DOCUMENT },
             // UE's class colours for the animation family, so a folder of rig content reads as one family.
             FileTypeInfo{
                  FileType::SkinnedMesh, "Skeletal Mesh", { 0.90f, 0.35f, 0.90f, 1.00f }, ICON_MDI_HUMAN },
             FileTypeInfo{ FileType::Skeleton, "Skeleton", { 0.41f, 0.71f, 0.80f, 1.00f }, ICON_MDI_BONE },
             FileTypeInfo{ FileType::Animation, "Animation", { 0.31f, 0.70f, 0.28f, 1.00f }, ICON_MDI_RUN },
             FileTypeInfo{
                  FileType::ControlRig, "Control Rig", { 0.20f, 0.45f, 0.95f, 1.00f }, ICON_MDI_HUMAN_HANDSUP },
             FileTypeInfo{ FileType::AnimGraph, "Anim Graph", { 0.80f, 0.55f, 0.20f, 1.00f }, ICON_MDI_SITEMAP },
             FileTypeInfo{
                  FileType::Retarget, "Retarget", { 0.95f, 0.50f, 0.60f, 1.00f }, ICON_MDI_SWAP_HORIZONTAL },
             FileTypeInfo{ FileType::FoliageType, "Foliage Type", { 0.30f, 0.75f, 0.35f, 1.00f }, ICON_MDI_TREE },
             FileTypeInfo{
                  FileType::StringTable, "String Table", { 0.60f, 0.60f, 0.85f, 1.00f }, ICON_MDI_TRANSLATE },
             FileTypeInfo{ FileType::CookedWorld, "Cooked World", { 0.50f, 0.50f, 0.55f, 1.00f }, ICON_MDI_MAP },
             FileTypeInfo{ FileType::Skybox, "Skybox", { 0.82f, 0.18f, 0.30f, 1.00f }, ICON_MDI_IMAGE_FILTER_HDR },
             FileTypeInfo{ FileType::WaterWaves, "Water Waves", { 0.20f, 0.55f, 0.85f, 1.00f }, ICON_MDI_WAVES },
        };

        constexpr bool RowsAreInEnumOrder()
        {
            if ( kRows.size() != static_cast<std::size_t>( kLastFileType ) + 1 )
                return false;
            for ( std::size_t i = 0; i < kRows.size(); ++i )
                if ( static_cast<std::size_t>( kRows[i].Type ) != i )
                    return false;
            return true;
        }
        static_assert( RowsAreInEnumOrder(), "kRows needs exactly one row per FileType, in enum order" );
    } // namespace

    const FileTypeInfo& FileTypeInfoOf( FileType type )
    {
        return kRows[static_cast<std::size_t>( type )];
    }
} // namespace Desert::Editor
