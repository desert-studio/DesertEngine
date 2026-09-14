#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <rflcpp/rfl/Generic.hpp>

namespace Desert::Reflection
{
    // The category of a reflected field. Drives both editor widget selection and (later) automatic
    // shader upload. Kept free of glm/asset dependencies so the reflection core stays standalone.
    enum class FieldType : uint8_t
    {
        Unknown = 0,
        Bool,
        Int,
        UInt,
        Float,
        Double,
        String,
        Vec2,
        Vec3,
        Vec4,
        Enum,
        Struct,
        AssetHandle,
    };

    // Editor/codegen metadata extracted from PROPERTY(...) attributes.
    struct PropertyMetadata
    {
        std::string DisplayName; // PROPERTY(DisplayName("..."))  — empty → use field name
        std::string Category;    // PROPERTY(Category("..."))     — empty → "Default"
        std::string Tooltip;     // PROPERTY(Tooltip("..."))      — hover help on the field row
        std::string Header;      // PROPERTY(Header("..."))       — section label drawn above the field

        bool  HasRange = false;  // PROPERTY(Range(min,max))
        float RangeMin = 0.0f;
        float RangeMax = 0.0f;

        bool        IsColor   = false; // PROPERTY(Color)               → color editor
        bool        IsAsset   = false; // PROPERTY(Asset<TextureAsset>) → asset picker
        std::string AssetType;         // reflected asset type name (e.g. "TextureAsset")
        bool        Thumbnail = false; // PROPERTY(Thumbnail)
        bool        ReadOnly  = false; // PROPERTY(ReadOnly)
        bool        Hidden    = false; // PROPERTY(Hidden)

        // PROPERTY(Length) — the field is a distance in world units, i.e. CENTIMETRES (one world unit is
        // one centimetre everywhere; see Common/Core/Units.hpp and docs/UNITS.md). The editor labels it
        // "cm" and drags it a centimetre at a time; Range(min,max) is in the same units.
        bool IsLength = false;

        // PROPERTY(Units("deg")) — the quantity this number is in. The editor appends the suffix and
        // picks a drag step that suits it. Purely presentational: NO value is ever converted, the stored
        // number (and any Range) is already in these units. Length is exactly Units("cm") for world
        // distances, kept as its own flag because the unit is an engine-wide invariant.
        std::string Units;

        // PROPERTY(Advanced) — the field folds under an "Advanced" node at the end of its category
        // instead of sitting in the main list. For things that exist but are rarely touched.
        bool Advanced = false;

        // PROPERTY(Summary) — the field feeds the one-line summary drawn next to the component's header,
        // so a COLLAPSED component still says what it is ("Point · 1000 cm · warm").
        bool Summary = false;

        // PROPERTY(Color, Temperature) — the colour row also gets a colour-TEMPERATURE (Kelvin) slider
        // that writes the RGB. The component stores only the resulting colour: Kelvin is an input to it,
        // not a second source of truth.
        bool Temperature = false;

        // PROPERTY(Preview) — an asset slot shows its content INLINE instead of only on hover. For slots
        // whose content is the point (a sprite, a decal); a long list of texture maps is better left as
        // names.
        bool Preview = false;

        // PROPERTY(EditCondition("Foo")) — the row is greyed while the bool field `Foo` of the SAME block
        // is false ("!Foo" inverts it). Unlike Hidden the field stays VISIBLE: the setting exists, it just
        // has no effect yet, and hiding it would only make people wonder where it went.
        std::string EditCondition;
    };

    struct TypeInfo; // fwd

    struct EnumValue
    {
        std::string Name;
        int64_t     Value = 0;
    };

    /// THE BYTES A MEMBER OCCUPIES INSIDE ITS STRUCT — not the number of elements it holds.
    ///
    /// Spelled as a named function rather than as `sizeof( T::Member )` at each of the ~600 generated
    /// field rows for one reason and it is not the diagnostic: `sizeof` of a `std::string` member reads,
    /// at a glance and to a static analyser alike, as somebody who meant `.size()`. It is not — this
    /// number sits beside `offsetof` and the pair describes where the member LIVES, which is what the
    /// property editor uses to address it. clang-tidy's `bugprone-sizeof-container` says the same thing
    /// seventeen times about Reflection.gen.cpp, and every one of those is a false positive; naming the
    /// quantity answers the check instead of muting it, and the answer is then also readable by a person.
    template <typename Member>
    [[nodiscard]] constexpr std::size_t FieldFootprint() noexcept
    {
        return sizeof( Member );
    }

    struct FieldInfo
    {
        std::string      Name;          // C++ field name
        FieldType        Type = FieldType::Unknown;
        std::size_t      Offset = 0;    // offsetof within the owning type
        std::size_t      Size   = 0;    // sizeof the field
        std::string      TypeName;      // C++ type spelling (for struct/enum/asset resolution)
        PropertyMetadata Meta;

        // For FieldType::Struct — resolved lazily from the registry by TypeName.
        const TypeInfo*         StructType = nullptr;
        // For FieldType::Enum.
        std::vector<EnumValue>  EnumValues;

        // Containers (std::vector<...>). The byte-offset serializer can't iterate/resize a vector
        // generically (it needs the element type at compile time), so the codegen emits typed lambdas
        // here. When set, the serializer routes this field through them instead of the switch above.
        bool                                                    IsContainer = false;
        std::function<rfl::Generic( const void* /*field*/ )>    SerializeContainer;
        std::function<void( void* /*field*/, const rfl::Generic& )> DeserializeContainer;

        const std::string& DisplayName() const
        {
            return Meta.DisplayName.empty() ? Name : Meta.DisplayName;
        }
    };

    struct TypeInfo
    {
        std::string            Name;
        std::size_t            Size = 0;
        std::vector<FieldInfo> Fields;

        // Returns a pointer to a process-wide default-constructed instance of the type (member initializers
        // give it the "factory defaults"), or nullptr if the codegen didn't provide one. Used by the editor's
        // reset-to-default: a field's default value is `GetDefaultInstance() + field.Offset`. Set via
        // TypeBuilder::WithDefault<T>() from the generated reflection code.
        const void* ( *GetDefaultInstance )() = nullptr;
    };
} // namespace Desert::Reflection
