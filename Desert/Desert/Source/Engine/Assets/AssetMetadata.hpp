#pragma once

#include "Common.hpp"

#include <Common/Core/Core.hpp>

#include <cstddef>
#include <functional>
#include <string>

namespace Desert::Assets
{
    struct AssetMetadata
    {
        // Every field has a default, and the defaults spell "no asset". IsValid() below tests the handle
        // against 0; leaving Handle without an initializer made a default-constructed metadata report
        // itself VALID back when UUID's default was random, and left AssetType reading uninitialized
        // memory.
        Common::UUID     Handle = Common::UUID::Null();
        Common::Filepath Filepath;
        AssetTypeID      AssetType = AssetTypeID::Unknown;
        /*std::unordered_map<std::string, std::variant<int, float, std::string>> AdditionalData;

        template <typename T>
        void SetAdditionalData( const std::string& key, const T& value )
        {
            AdditionalData[key] = value;
        }

        template <typename T>
        T GetAdditionalData( const std::string& key, const T& defaultValue = T{} ) const
        {

        }*/

        bool IsValid() const
        {
            return Handle != 0 && ( !Filepath.empty() ) && AssetType != AssetTypeID::Unknown;
        }
    };

    // WHICH ASSET A QUESTION IS ABOUT — as a TYPE, so that it cannot be asked any other way.
    //
    // THE DEFECT THIS CLOSES. The registry answered one question two ways. `AssetManager::CreateAsset`
    // deduplicated on the path-derived stable key — spelling-independent by construction — while
    // `AssetManager::FindByPath` scanned the records comparing `AssetMetadata::Filepath` VERBATIM. Both
    // meant "does the registry already hold this file, as this type?", and they disagreed the moment two
    // spellings of one file met, which in this engine is the normal case rather than an odd one: every
    // content root turns ABSOLUTE when a `.deproj` is opened (Constants::Path::SetProjectRoot), so the
    // preloader registers `<home>/Game/Cooked/Meshes/base.stmesh` while the scene that references it says
    // `Cooked/Meshes/base.stmesh`.
    //
    // Measured cost of the disagreement, on the reproducer scene: `FindByPath` missed, `CreateAsset` hit,
    // and the caller therefore handed `MeshService::Register` the preloader's UNPARSED shell believing it
    // had just created it. A StaticMesh was built from 0 vertices and 0 submeshes and cached under the
    // handle; `Get` answered zero submeshes 91 times in one 90-frame run and the frame was empty. Two
    // other sites had each already grown a hand-written detour around the same disagreement, with a
    // comment naming it, rather than closing it.
    //
    // WHY A CLASS AND NOT A FREE FUNCTION RETURNING std::string. A string key is a convention: the map
    // that holds it will accept any string at all, so a raw path spelled into it is a compiling,
    // plausible, wrong key — which is exactly the mistake being retired. This type has ONE constructor
    // and it takes the two things an identity is made of. There is deliberately no constructor from a
    // string and no implicit conversion from a path, so `m_PathLookup.find( somePath )` does not compile
    // and the older question cannot be re-introduced by accident.
    //
    // WHY THE TYPE IS PART OF THE KEY: two asset classes are allowed to sit on one path (the handle
    // derivation deliberately gives them the same number) and they are still two records.
    //
    // THIS IS THE ENGINE'S ANSWER EVERYWHERE ELSE ALREADY, which is what makes it the shape to converge
    // on rather than a preference. `FontService::RegisterFont`, `VideoService::RegisterVideo` and
    // `IconService::RegisterIcon` each key their registry on `AssetHandle::FromCookedPath` — i.e. on this
    // same derivation — and each says in its own comment that hashing the raw string gave one file two
    // handles. Three small registries had paid for it and fixed it; the main one had not.
    //
    // WHY THE STABLE KEY AND NOT THE HANDLE: Texture2D and Material overwrite their handle from an id
    // stored inside their file during Load, so a not-yet-loaded lookup record carries the path-derived
    // value and would never match a loaded one.
    //
    // COST. Building one is one `StableKeyForPath` — path algebra and a couple of allocations. That is
    // affordable only because it is asked ONCE PER QUESTION rather than once per comparison: computing it
    // inside a scan cost 56.9 s over a 2000-asset preload against 207 ms for the raw-path scan it
    // replaced, while asking it once and hashing makes the same preload 30 ms.
    class AssetKey final
    {
    public:
        AssetKey( const Common::Filepath& filepath, AssetTypeID assetType )
             : m_Value( Common::AssetHandle::StableKeyForPath( filepath ) + '#' +
                        std::to_string( static_cast<int>( assetType ) ) )
        {
        }

        explicit AssetKey( const AssetMetadata& metadata ) : AssetKey( metadata.Filepath, metadata.AssetType )
        {
        }

        // For log lines and for tests. It is NOT a way back into the map: nothing takes a string key.
        [[nodiscard]] const std::string& Value() const noexcept
        {
            return m_Value;
        }

        bool operator==( const AssetKey& other ) const noexcept
        {
            return m_Value == other.m_Value;
        }

    private:
        std::string m_Value;
    };
} // namespace Desert::Assets

namespace std
{
    template <>
    struct hash<Desert::Assets::AssetKey>
    {
        std::size_t operator()( const Desert::Assets::AssetKey& key ) const noexcept
        {
            return std::hash<std::string>{}( key.Value() );
        }
    };
} // namespace std