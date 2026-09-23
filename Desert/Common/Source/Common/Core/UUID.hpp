#pragma once

#include <cstdint>
#include <iostream>
#include <string>

namespace Common
{
    // A 64-bit identity.
    //
    // THE DEFAULT-CONSTRUCTED VALUE IS NULL (0), AND RANDOMNESS IS ASKED FOR BY NAME.
    //
    // It used to be the other way round: `UUID()` minted a random id and the header carried a warning that
    // `UUID{}` must therefore never be used as a "none/not-found" sentinel. A warning is not a mechanism.
    // Every C++ idiom that reaches for a zero — a struct member written without an initializer, `= {}`,
    // `value_or({})`, `!= {}`, a partially aggregate-initialized struct — silently produced a fresh random
    // number instead, and the resulting bug is invisible: nothing throws, nothing logs, a comparison that
    // was meant to read "is this unset?" simply answers "no" forever. That defect shipped in the scene
    // deserializer, in both prefab instantiators and in two ECS components at once.
    //
    // So the safe value is now the one you get for free, and the dangerous one has to be spelled
    // `UUID::Generate()`. Callers that genuinely want a fresh identity (a new entity, a new material GUID,
    // a runtime-only image key) say so at the call site, where a reader can check the intent.
    class UUID
    {
    public:
        // Null (0) — "no id". Cheap, deterministic, and the same value in every process.
        constexpr UUID() noexcept : m_UUID( 0 )
        {
        }

        explicit UUID( uint64_t uuid );
        explicit UUID( const std::string& uuidStr );

        // DEFAULTED, not hand-written. The copy constructor used to be a user-provided
        // `UUID( const UUID& other ) : m_UUID( other.m_UUID ) {}` in the .cpp, which does precisely what
        // the implicit one does — and having it made the implicitly-declared copy ASSIGNMENT deprecated
        // (C++11 [depr.impldec], `-Wdeprecated-copy-with-user-provided-copy`). A type whose whole content
        // is one `uint64_t` should declare all four or none; this declares them, so the deprecation is
        // gone and nothing about copying a UUID has changed.
        UUID( const UUID& )            = default;
        UUID& operator=( const UUID& ) = default;
        UUID( UUID&& )                 = default;
        UUID& operator=( UUID&& )      = default;

        // A fresh random identity. The ONLY way to get one — see the class comment for why it is not the
        // default.
        static UUID Generate();

        // Explicit "no UUID" value (0). Identical to the default ctor; kept because `UUID::Null()` reads as
        // an intent at a call site where `{}` reads as an oversight.
        static UUID Null()
        {
            return UUID();
        }

        bool IsNull() const
        {
            return m_UUID == 0;
        }

        const std::string ToString() const
        {
            return std::to_string( m_UUID ); // TODO: Cache
        }

        operator uint64_t()
        {
            return m_UUID;
        }
        operator const uint64_t() const
        {
            return m_UUID;
        }

    private:
        uint64_t m_UUID;
    };
} // namespace Common

namespace std
{

    template <>
    struct hash<Common::UUID>
    {
        std::size_t operator()( const Common::UUID& uuid ) const
        {
            // The value is either a random draw or an FNV-1a digest of a path, so it is already well
            // spread and serves as its own hash.
            return uuid;
        }
    };
} // namespace std