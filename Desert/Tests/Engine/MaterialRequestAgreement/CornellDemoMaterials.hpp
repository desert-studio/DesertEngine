#pragma once

#include <Engine/Assets/MaterialParamDiff.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace Desert::Tests::CornellDemo
{
    // WHAT THE CORNELL DEMO'S MATERIALS MUST SAY — the expected values of the shipped CB_*.demat files.
    //
    // The files under Projects/Desert/Content/Materials are the content (CornellDemo.desce references them);
    // this table is what their author asked for: roughness 0.9 and no metalness on every wall. It lives with
    // the suite that checks it, because the editor no longer builds the demo — the scene and its materials
    // are files, and CB_Red once shipped as a chrome mirror (MetallicFactor 1.0, RoughnessFactor 0.0) that
    // only a human looking at a picture could catch. The test asserts the files still say what this says.
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
} // namespace Desert::Tests::CornellDemo
