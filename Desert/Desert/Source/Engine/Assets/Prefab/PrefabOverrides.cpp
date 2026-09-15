#include <Engine/Assets/Prefab/PrefabOverrides.hpp>

#include <rflcpp/rfl/json.hpp>

namespace Desert::Assets
{
    namespace
    {
        // TWO PAYLOADS ARE THE SAME WHEN THEIR TEXT IS. rfl::Generic is a variant tree with no equality
        // operator, and writing a hand-rolled recursive comparison for it would be a second definition of
        // "same value" living beside the writer's — the shape this file exists to avoid. The text is the
        // definition the file itself uses, and both sides of every comparison are produced by the same
        // reflected walk over the same type, so key order is a property of the type rather than of the
        // caller.
        [[nodiscard]] bool SameGeneric( const rfl::Generic& a, const rfl::Generic& b )
        {
            return rfl::json::write( a ) == rfl::json::write( b );
        }

        template <typename T>
        [[nodiscard]] bool SameOptional( const std::optional<T>& a, const std::optional<T>& b )
        {
            if ( a.has_value() != b.has_value() )
            {
                return false;
            }
            return !a.has_value() || *a == *b;
        }
    } // namespace

    std::optional<PrefabOverrideData> DiffPrefabEntity( const EntityData& base, const EntityData& live,
                                                        std::vector<Common::UUID> path, PrefabDiffReport* report )
    {
        PrefabOverrideData over;
        over.Path     = std::move( path );
        bool anything = false;

        if ( !SameOptional( base.Tag, live.Tag ) && live.Tag.has_value() )
        {
            over.Tag = live.Tag;
            anything = true;
        }

        // The transform is three independent fields and is compared as three: a prefab authored without a
        // Scale and an instance that only moved must record the translation alone, or the absent scale
        // would be pinned to whatever the live entity happened to hold.
        if ( !SameOptional( base.Translation, live.Translation ) && live.Translation.has_value() )
        {
            over.Translation = live.Translation;
            anything         = true;
        }
        if ( !SameOptional( base.Rotation, live.Rotation ) && live.Rotation.has_value() )
        {
            over.Rotation = live.Rotation;
            anything      = true;
        }
        if ( !SameOptional( base.Scale, live.Scale ) && live.Scale.has_value() )
        {
            over.Scale = live.Scale;
            anything   = true;
        }

        for ( const auto& [key, value] : live.Components )
        {
            const auto inBase = base.Components.get( key );
            if ( inBase.has_value() && SameGeneric( inBase.value(), value ) )
            {
                continue;
            }
            over.Components[key] = value;
            anything             = true;
        }

        // A component the instance DROPPED. The override is applied on top of the instantiated base, so
        // there is no value that means "and remove this one" — the base's copy simply comes back. Counted
        // here and named by the caller; inventing a tombstone key for it would put a second spelling of
        // "no component" into every file that has none.
        if ( report != nullptr )
        {
            for ( const auto& [key, value] : base.Components )
            {
                (void)value;
                if ( !live.Components.get( key ).has_value() )
                {
                    ++report->RemovedComponents;
                }
            }
        }

        if ( !anything )
        {
            return std::nullopt;
        }
        return over;
    }

    EntityData OverrideAsEntityData( const PrefabOverrideData& over )
    {
        EntityData data;
        data.Tag         = over.Tag;
        data.Translation = over.Translation;
        data.Rotation    = over.Rotation;
        data.Scale       = over.Scale;
        for ( const auto& [key, value] : over.Components )
        {
            data.Components[key] = value;
        }
        return data;
    }

    void LayerOverrideOntoRecord( EntityData& base, const PrefabOverrideData& over )
    {
        if ( over.Tag )
        {
            base.Tag = over.Tag;
        }
        if ( over.Translation )
        {
            base.Translation = over.Translation;
        }
        if ( over.Rotation )
        {
            base.Rotation = over.Rotation;
        }
        if ( over.Scale )
        {
            base.Scale = over.Scale;
        }
        for ( const auto& [key, value] : over.Components )
        {
            base.Components[key] = value;
        }
    }

    std::string PrefabPathKey( std::span<const Common::UUID> path )
    {
        std::string key;
        for ( const Common::UUID& id : path )
        {
            key += id.ToString();
            key += '/';
        }
        return key;
    }
} // namespace Desert::Assets
