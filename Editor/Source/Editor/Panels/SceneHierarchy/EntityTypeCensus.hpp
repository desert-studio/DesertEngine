#pragma once

#include <array>
#include <cstddef>

namespace Desert::Editor
{
    // THE OUTLINER'S TYPE COLUMN, as data.
    //
    // It used to be a chain of `if (HasComponent<X>()) return "XActor";` and a hard-coded 110px column,
    // and the two disagreed on every row: "StaticMeshActor" and "DirectionalLight" do not fit in 110px at
    // the editor's font, so the column that exists to say WHAT a row is was clipped on essentially every
    // entity in the Starter scene.
    //
    // The fix is not a bigger number. It is making the two sides of the relation ONE thing: the column is
    // sized from this table, the names are drawn from this table, and a type added tomorrow widens the
    // column by existing. The enum and the table are pinned to each other by a static_assert below, so
    // adding a kind without a row does not compile — the census cannot silently fall behind (which is the
    // failure mode this project has now paid for seven times in one day; see the outliner's own history).
    //
    // Deliberately free of ImGui and of the ECS: the width relation is then testable on its own, without
    // a font atlas or a registry (see Desert/Tests/Editor/OutlinerTypeColumn).

    enum class EntityTypeKind
    {
        Folder = 0,
        Camera,
        DirectionalLight,
        PointLight,
        SpotLight,
        SkyAtmosphere,
        Skybox,
        Landscape,
        SkinnedMesh,
        StaticMesh,
        Text,
        Actor,

        Count
    };

    // A colour FAMILY, not a colour per type: the eye is meant to sort a long list into meshes / lights /
    // environment / gameplay at a glance, and twelve distinct hues would sort it into nothing. Stored as
    // plain floats so this header stays independent of ImGui.
    struct EntityTypeInfo
    {
        const char* Name;
        float       R, G, B;
    };

    namespace EntityTypePalette
    {
        // Sampled from the approved mock's outliner: mesh blue, light amber, environment teal, gameplay
        // green, folder grey.
        inline constexpr float MeshR = 0.470f, MeshG = 0.745f, MeshB = 1.000f;    // 120,190,255
        inline constexpr float LightR = 1.000f, LightG = 0.804f, LightB = 0.431f; // 255,205,110
        inline constexpr float EnvR = 0.510f, EnvG = 0.824f, EnvB = 0.784f;       // 130,210,200
        inline constexpr float PlayR = 0.588f, PlayG = 0.863f, PlayB = 0.588f;    // 150,220,150
        inline constexpr float DimR = 0.659f, DimG = 0.659f, DimB = 0.659f;       // 168,168,168
    } // namespace EntityTypePalette

    inline constexpr std::array<EntityTypeInfo, static_cast<std::size_t>( EntityTypeKind::Count )> kEntityTypes = {
         {
              // Names are the reader's words, not the C++ type's: the column answers "what is this thing",
              // and "StaticMeshActor" spends nine characters saying "Actor" to someone looking at a list of
              // actors. The Details panel still shows the component names.
              { "Folder", EntityTypePalette::DimR, EntityTypePalette::DimG, EntityTypePalette::DimB },
              { "Camera", EntityTypePalette::PlayR, EntityTypePalette::PlayG, EntityTypePalette::PlayB },
              { "Directional Light", EntityTypePalette::LightR, EntityTypePalette::LightG,
                EntityTypePalette::LightB },
              { "Point Light", EntityTypePalette::LightR, EntityTypePalette::LightG, EntityTypePalette::LightB },
              { "Spot Light", EntityTypePalette::LightR, EntityTypePalette::LightG, EntityTypePalette::LightB },
              { "Sky Atmosphere", EntityTypePalette::EnvR, EntityTypePalette::EnvG, EntityTypePalette::EnvB },
              { "Sky Box", EntityTypePalette::EnvR, EntityTypePalette::EnvG, EntityTypePalette::EnvB },
              { "Landscape", EntityTypePalette::EnvR, EntityTypePalette::EnvG, EntityTypePalette::EnvB },
              { "Skinned Mesh", EntityTypePalette::MeshR, EntityTypePalette::MeshG, EntityTypePalette::MeshB },
              { "Static Mesh", EntityTypePalette::MeshR, EntityTypePalette::MeshG, EntityTypePalette::MeshB },
              { "Text", EntityTypePalette::MeshR, EntityTypePalette::MeshG, EntityTypePalette::MeshB },
              { "Actor", EntityTypePalette::PlayR, EntityTypePalette::PlayG, EntityTypePalette::PlayB },
         } };

    static_assert( kEntityTypes.size() == static_cast<std::size_t>( EntityTypeKind::Count ),
                   "Every EntityTypeKind needs a row in kEntityTypes: the outliner sizes its Type column "
                   "from this table, so a kind without a row would be drawn in a column that never "
                   "measured it." );

    [[nodiscard]] inline constexpr const EntityTypeInfo& EntityTypeOf( EntityTypeKind kind ) noexcept
    {
        return kEntityTypes[static_cast<std::size_t>( kind )];
    }

    // Width the Type column must have to print ANY name in the census without an ellipsis.
    //
    // `measure` is the text measurement — ImGui::CalcTextSize().x in the editor, a stub in the test. It is
    // a parameter rather than a call so the relation "the column fits the widest name" can be asserted
    // without a font atlas, which is the only reason it was ever allowed to be false.
    template <typename MeasureFn>
    [[nodiscard]] float TypeColumnWidth( MeasureFn&& measure, float padding )
    {
        float widest = 0.0f;
        for ( const EntityTypeInfo& info : kEntityTypes )
        {
            const float w = measure( info.Name );
            if ( w > widest )
                widest = w;
        }
        return widest + padding;
    }
} // namespace Desert::Editor
