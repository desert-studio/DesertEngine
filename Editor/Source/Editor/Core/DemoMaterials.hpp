#pragma once

#include <Engine/Assets/MaterialParamDiff.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace Desert::Editor::MaterialAssetUtils
{
    // WHAT THE DEMO MATERIALS ARE, AS DATA RATHER THAN AS ARGUMENTS BURIED IN A BUILDER.
    //
    // These values used to live only as literals inside EditorLayer::BuildCornellShowcase, passed
    // straight into a find-or-create that never looked at them again once a file existed. That made
    // them unreachable in two senses at once: the builder could not apply them, and NOTHING COULD READ
    // THEM to check the file against — so `CB_Red.demat` shipped as a chrome mirror (MetallicFactor 1.0,
    // RoughnessFactor 0.0) against an author that asked for roughness 0.9 and no metalness, and the only
    // way to find out was to render the scene and notice a wall was black.
    //
    // As a table it is content, the builder is mechanism, and the agreement between the table and the
    // files on disk is a plain data question that Desert/Tests/Engine/MaterialRequestAgreement answers
    // offline, every sweep, with no editor and no GPU. That is the whole point of moving it: a defect
    // that previously needed a human looking at a picture now needs a test that already runs.
    //
    // It is NOT a mirror of the .demat files — it is their SOURCE. The files are generated from this on
    // first launch, and the test asserts the generated copies still say what this says.
    struct DemoMaterial
    {
        std::string_view                          Name;
        std::vector<Assets::MaterialParamRequest> Params;
        // The template the material is authored on, by its shader header GUID (the identity a .demat states).
        // EMPTY = the project's `Default Surface` template: a parameter belongs to ONE template's Properties
        // block, so a material whose params are another template's (glass: IOR, GlassTint) must name it, or
        // a re-authored file would carry params its template has no row for.
        std::string_view Template = {};
    };

    // StaticMeshGlass.shader's header GUID: the glass surface template (BlendMode Translucent).
    inline constexpr std::string_view kGlassTemplateGuid = "2cd21e52ff88b0c7a42c25d9a6ab4238";

    // The Cornell-box showcase set. Roughness 0.9 on every opaque surface is the fixture's defining
    // property and not a taste: a Cornell box's walls are diffuse by definition, which is what makes the
    // two side walls comparable to one another and the box a usable regression fixture at all.
    [[nodiscard]] inline const std::vector<DemoMaterial>& CornellDemoMaterials()
    {
        static const std::vector<DemoMaterial> materials = {
             { "CB_White",
               { { "AlbedoColor", { 0.82f, 0.82f, 0.80f, 1.0f } },
                 { "RoughnessFactor", { 0.9f, 0.0f, 0.0f, 0.0f } } } },
             { "CB_Red",
               { { "AlbedoColor", { 0.85f, 0.10f, 0.10f, 1.0f } },
                 { "RoughnessFactor", { 0.9f, 0.0f, 0.0f, 0.0f } } } },
             { "CB_Green",
               { { "AlbedoColor", { 0.10f, 0.70f, 0.15f, 1.0f } },
                 { "RoughnessFactor", { 0.9f, 0.0f, 0.0f, 0.0f } } } },
             { "CB_Orange",
               { { "AlbedoColor", { 0.95f, 0.50f, 0.08f, 1.0f } },
                 { "RoughnessFactor", { 0.9f, 0.0f, 0.0f, 0.0f } } } },
             { "CB_Glass",
               { { "IOR", { 1.5f, 0.0f, 0.0f, 0.0f } }, { "GlassTint", { 0.75f, 0.9f, 1.0f, 1.0f } } },
               kGlassTemplateGuid },
        };
        return materials;
    }

    // The params of one demo material by name. Refuses loudly rather than returning an empty set: an
    // empty parameter list is a legal request ("author a material with nothing set"), so a typo'd name
    // must not be able to look like one — that is the same empty-successful-answer shape the contract
    // forbids, one level down.
    [[nodiscard]] inline const DemoMaterial* FindDemoMaterial( std::string_view name )
    {
        for ( const auto& material : CornellDemoMaterials() )
            if ( material.Name == name )
                return &material;
        return nullptr;
    }
} // namespace Desert::Editor::MaterialAssetUtils
