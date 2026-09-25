#pragma once

#include <Engine/Reflection/ReflectionTypes.hpp>

#include <rflcpp/rfl/Generic.hpp>

#include <cstdint>
#include <functional>
#include <string>

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
    // object bytes at each field offset into an rfl::Generic tree. This is the single code path that makes
    // "everything reflected serializes the same way" true: materials, light/camera data blocks, and any
    // future reflected struct all round-trip through here without a hand-written mirror struct.
    //
    // When the reflection core gains enum/nested/container support (#4), this serializer picks it up for
    // free — no per-type serialization code to update.

    // Serializes a reflected object (described by `type`) into a generic JSON object. When `resolver` is
    // given, AssetHandle fields are written as path strings (else as raw uint64).
    rfl::Generic::Object SerializeReflected( const TypeInfo& type, const void* obj,
                                             const AssetResolver* resolver = nullptr );

    // Deserializes from a generic JSON object into a reflected object, in place. Fields missing from
    // `src` keep their current (default) value — this gives forward/backward field compatibility. An
    // AssetHandle value that is a string is resolved via `resolver`; a number is read as a raw handle.
    void DeserializeReflected( const TypeInfo& type, void* obj, const rfl::Generic::Object& src,
                               const AssetResolver* resolver = nullptr );
} // namespace Desert::Reflection
