#pragma once

#include <Engine/Reflection/ReflectionTypes.hpp>

#include <Common/Json/Document.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>

namespace Desert::Reflection
{
    // Bridges reflected AssetHandle fields to their on-disk form. The reflection core can't know about
    // the AssetManager/asset services, so the caller (ComponentRegistry) injects this. When present, an
    // AssetHandle field (de)serializes as a stable PATH STRING (backward-compatible with the old custom
    // serializers) instead of a raw runtime uint64. `assetType` comes from PROPERTY(Asset<...>) metadata
    // so the resolver can dispatch per asset type (skybox vs mesh vs material).
    //
    // INVARIANT: Core::MakeAssetResolver is the only place one is built, and it assigns all three members
    // unconditionally. A resolver that exists therefore has every member callable — test the POINTER for
    // null (it is optional and defaults to nullptr), never an individual std::function for emptiness.
    struct AssetResolver
    {
        std::function<std::string( uint64_t handle, const std::string& assetType )>      ToPath;
        std::function<uint64_t( const std::string& path, const std::string& assetType )> FromPath;
        // Asset-database resolution: validates a persisted stable GUID (ensuring the asset is
        // loaded + registered in its service) and returns it, or 0 when unknown — the caller
        // then falls back to FromPath.
        std::function<uint64_t( uint64_t guid, const std::string& assetType )> FromGuid;
        // The header GUID text of the asset `handle` names ("" for 0 or an asset with no identity). A
        // SkyboxAsset or TextureAsset field is written as {"Guid": ToGuid, "Path": ToPath} and read back by
        // ResolveGuidRef.
        std::function<std::string( uint64_t handle, const std::string& assetType )> ToGuid;
    };

    // AN ASSET REFERENCE IS NAMED BY THE ASSET'S HEADER GUID; the path beside it is only a locator. The
    // GUID's handle (HandleForGuid) is looked up first; on a miss the path loads it, and the file found
    // there must BE that asset: a path that now holds a different asset leaves the reference empty with
    // both named. No GUID and no path is an empty reference; a path with no GUID is refused by name.
    uint64_t ResolveGuidRef( const AssetResolver& resolver, const std::string& text, const std::string& path,
                             const char* type, const std::string& what );

    // Generic, reflection-driven (de)serialization. Walks a TypeInfo's fields and reads/writes the raw
    // object bytes at each field offset into a JSON value tree. This is the single code path that makes
    // "everything reflected serializes the same way" true: materials, light/camera data blocks, and any
    // future reflected struct all round-trip through here without a hand-written mirror struct.

    // Serializes a reflected object (described by `type`) into a JSON object. When `resolver` is given,
    // AssetHandle fields are written as path strings (else as raw uint64).
    Common::Json::Object SerializeReflected( const TypeInfo& type, const void* obj,
                                             const AssetResolver* resolver = nullptr );

    // Deserializes the object `src` into a reflected object, in place, under THE WRONG-TYPE RULE
    // (Common/Json/Document.hpp): a field missing from `src` keeps its current value (forward/backward field
    // compatibility); a field of the wrong type — a string where a float belongs, a vec of the wrong length,
    // an integer outside the field's range — also keeps its current value and appends an Issue naming its
    // full path, and the walk continues. `src` that is not an object is one Issue and nothing is read. An
    // AssetHandle value that is a string is resolved via `resolver`; an integer is read as a raw handle.
    // Root `src` at the component's path so an Issue names the entity and component, not just the field.
    void DeserializeReflected( const TypeInfo& type, void* obj, const Common::Json::Node& src,
                               Common::Json::Issues& issues, const AssetResolver* resolver = nullptr );

    // THE CODEGEN'S HALVES OF A std::vector FIELD (FieldInfo::SerializeContainer / DeserializeContainer).
    // Each element is written as JSON spells `Stored`. A 64-bit handle is stored as `std::uint64_t`, which
    // the value tree keeps in its int64 alternative (reinterpreted, so ids above 2^63 survive) and which
    // ReadContainer reinterprets back the same way.
    template <typename Vector, typename Stored = typename Vector::value_type>
    [[nodiscard]] Common::Json::Value WriteContainer( const void* field )
    {
        Common::Json::Value::Array array;
        for ( const auto& element : *static_cast<const Vector*>( field ) )
            array.push_back( Common::Json::Detail::ToValue( static_cast<Stored>( element ) ) );
        return Common::Json::Value( std::move( array ) );
    }

    // Reads the array back under the wrong-type rule, and a vector is ONE field: any element of the wrong
    // type is an Issue (with its index in the path) and the WHOLE vector keeps its current value — a
    // vector with one element silently missing would shift every later index.
    template <typename Vector, typename Stored = typename Vector::value_type>
    void ReadContainer( void* field, const Common::Json::Node& src, Common::Json::Issues& issues )
    {
        if ( !src.ExpectKind( Common::Json::Kind::Array, issues ) )
            return;
        Vector            read;
        const std::size_t before = issues.size();
        src.ForEachElement(
             [&]( std::size_t, const Common::Json::Node& element )
             {
                 if constexpr ( std::is_same_v<Stored, std::uint64_t> )
                 {
                     std::int64_t raw = 0;
                     element.ReadValue( raw, issues );
                     read.push_back( typename Vector::value_type( static_cast<std::uint64_t>( raw ) ) );
                 }
                 else
                 {
                     Stored value{};
                     element.ReadValue( value, issues );
                     read.push_back( static_cast<typename Vector::value_type>( value ) );
                 }
             } );
        if ( issues.size() == before )
            *static_cast<Vector*>( field ) = std::move( read );
    }
} // namespace Desert::Reflection
