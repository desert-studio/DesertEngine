#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <Common/Core/ResultStr.hpp>
#include <Common/Json/Json.hpp>

namespace Common::Json
{
    class Node;
    struct Issue;
} // namespace Common::Json

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
        Entity, // an entity of a world: its id and the world it lives in (Value::EntityRef)
        Any,    // a parameter/result of a reflected function that takes whatever kind it is given (Godot's
                // Variant): the callee reads the Value's own Type(). Never a field's kind, never a Value's.
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

    class Value; // Value.hpp — what a container element travels as (FieldInfo::ContainerGet/Set)

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
        // here. When set, the serializer routes this field through them instead of the switch above. The
        // codegen plugs in WriteContainer/ReadContainer (ReflectionSerializer.hpp); the reader follows the
        // wrong-type rule and appends to the Issues it is given.
        bool                                                    IsContainer = false;
        std::function<Common::Json::Value( const void* /*field*/ )> SerializeContainer;
        std::function<void( void* /*field*/, const Common::Json::Node&, std::vector<Common::Json::Issue>& )>
             DeserializeContainer;
        // A std::vector of asset handles (codegen: ContainerHandles / AssignHandles in ReflectionSerializer.hpp).
        // Set, the serializer writes each element in the reference form of Meta.AssetType when it has a
        // resolver, exactly as a single handle field of that type; with no resolver the raw container above.
        std::function<std::vector<std::uint64_t>( const void* /*field*/ )>        ContainerHandles;
        std::function<void( void* /*field*/, const std::vector<std::uint64_t>& )> AssignHandles;

        // THE CONTAINER'S ELEMENTS, for any reader that is not the JSON serializer (a script language, a
        // property editor): codegen'd typed accessors over the std::vector (ContainerAccess.hpp), the element
        // travelling as a Value of the element's kind — an asset handle as its 64-bit UInt id. ElementType is
        // the element's category in the field vocabulary (AssetHandle for a vector of handles). ContainerGet
        // past the end is an empty Value; ContainerSet refuses an index past the end, a Value of another kind
        // and an integer outside the element's range, writing nothing.
        FieldType ElementType                                                   = FieldType::Unknown;
        std::size_t ( *ContainerSize )( const void* /*field*/ )                 = nullptr;
        void ( *ContainerResize )( void* /*field*/, std::size_t /*count*/ )     = nullptr;
        Value ( *ContainerGet )( const void* /*field*/, std::size_t /*index*/ ) = nullptr;
        bool ( *ContainerSet )( void* /*field*/, std::size_t /*index*/, const Value& /*value*/ ) = nullptr;

        const std::string& DisplayName() const
        {
            return Meta.DisplayName.empty() ? Name : Meta.DisplayName;
        }
    };

    /// One parameter or result of a reflected function: its name in the header, its category in the same
    /// vocabulary as a field's, and the C++ spelling (for a diagnostic and for a binding generator).
    struct ParamInfo
    {
        std::string Name;
        FieldType   Type = FieldType::Unknown;
        std::string TypeName;
    };

    /// FUNCTION(...) attributes, parsed by DesertHeaderTool exactly as PROPERTY(...) ones are.
    struct FunctionMetadata
    {
        bool        ScriptCallable = false; // FUNCTION(ScriptCallable) — a script language may bind and call it
        std::string Category;               // FUNCTION(Category("...")) — grouping in a browser or a palette
        std::string Tooltip;                // FUNCTION(Tooltip("..."))  — hover help
        std::string ScriptName;             // FUNCTION(ScriptName("...")) — the name a language binds (UE's
                                            // meta=(ScriptName)); empty = the C++ name
        bool ScriptMethod = false;          // FUNCTION(ScriptMethod) — a static whose first parameter is an
                                            // entity, bound as a method of the entity (UE's meta=(ScriptMethod))
    };

    /// THE CALL ITSELF, generated per function (FunctionThunk.hpp): unpacks the Values into the C++
    /// arguments, calls, packs the result. FunctionInfo::Invoke has checked the count, the kinds and `self`
    /// before it runs, so a thunk only refuses what only it can see (an integer outside its parameter's range).
    using FunctionThunk = Common::BoolResultStr ( * )( void* self, const Value* args, Value* rets );

    /// A REFLECTED FUNCTION — UE's UFunction, Godot's MethodBind: the one public layer through which ANY
    /// language calls C++. A language binding reads Params/Returns/Meta to build its side and calls Invoke;
    /// nothing in here knows that a language exists.
    struct FunctionInfo
    {
        std::string            Name;
        std::string            Owner; // registry name of the reflected type that declares it
        std::vector<ParamInfo> Params;
        std::vector<ParamInfo> Returns; // empty for void; one entry otherwise
        bool                   IsStatic = false;
        bool                   IsConst  = false;
        FunctionMetadata       Meta;
        FunctionThunk          Thunk = nullptr;

        /// Calls the function. `self` is the instance (nullptr for a static function, required otherwise);
        /// `args` holds exactly Params.size() values of exactly the parameters' kinds; `rets` has room for
        /// Returns.size() values. Every mismatch is refused with what was expected — a value of another kind
        /// is never converted here: converting is the CALLER'S language rule (Lua's number -> Float), not
        /// this layer's.
        Common::BoolResultStr Invoke( void* self, const Value* args, std::size_t argc, Value* rets ) const;

        /// The name a language binds the function under (Meta.ScriptName, else Name).
        [[nodiscard]] const std::string& BoundName() const
        {
            return Meta.ScriptName.empty() ? Name : Meta.ScriptName;
        }
    };

    /// EVENT(...) attributes, parsed by DesertHeaderTool exactly as FUNCTION(...) ones are.
    struct EventMetadata
    {
        std::string Category; // EVENT(Category("...")) — grouping in a browser or a palette
        std::string Tooltip;  // EVENT(Tooltip("..."))  — hover help
    };

    /// A REFLECTED EVENT — UE's sparse multicast delegate signature (FComponentHitSignature on
    /// UPrimitiveComponent::OnComponentHit): what an instance of the type announces, and the payload it carries.
    /// Only the description lives here; who listens to which instance is the subscriber table's
    /// (ECS/ComponentEvents.hpp), so a type whose events nobody binds costs nothing.
    struct EventInfo
    {
        std::string            Name;
        std::string            Owner; // registry name of the reflected type that declares it
        std::vector<ParamInfo> Params;
        EventMetadata          Meta;
    };

    struct TypeInfo
    {
        std::string               Name;
        std::string               ScriptName; // REFLECT(ScriptName("...")): the global a language binds the
                                              // type's static functions under; empty = Name
        std::size_t               Size = 0;
        std::vector<FieldInfo>    Fields;
        std::vector<FunctionInfo> Functions; // FUNCTION(...) members, in declaration order
        std::vector<EventInfo>    Events;    // EVENT(...) signatures, in declaration order

        /// The function named `name`, or nullptr. Names are unique within a type (the header tool refuses an
        /// overload: a language calls by name, and two C++ signatures under one name would be a guess).
        /// The name a language binds the type under (ScriptName, else Name).
        [[nodiscard]] const std::string& BoundName() const
        {
            return ScriptName.empty() ? Name : ScriptName;
        }

        [[nodiscard]] const FunctionInfo* FindFunction( std::string_view name ) const
        {
            for ( const FunctionInfo& function : Functions )
                if ( function.Name == name )
                    return &function;
            return nullptr;
        }

        /// The function a language binds as `name` (its BoundName), or nullptr.
        [[nodiscard]] const FunctionInfo* FindBoundFunction( std::string_view name ) const
        {
            for ( const FunctionInfo& function : Functions )
                if ( function.BoundName() == name )
                    return &function;
            return nullptr;
        }

        /// The event named `name`, or nullptr (names are unique within a type, as functions' are).
        [[nodiscard]] const EventInfo* FindEvent( std::string_view name ) const
        {
            for ( const EventInfo& event : Events )
                if ( event.Name == name )
                    return &event;
            return nullptr;
        }

        // Returns a pointer to a process-wide default-constructed instance of the type (member initializers
        // give it the "factory defaults"), or nullptr if the codegen didn't provide one. Used by the editor's
        // reset-to-default: a field's default value is `GetDefaultInstance() + field.Offset`. Set via
        // TypeBuilder::WithDefault<T>() from the generated reflection code.
        const void* ( *GetDefaultInstance )() = nullptr;
    };
} // namespace Desert::Reflection
