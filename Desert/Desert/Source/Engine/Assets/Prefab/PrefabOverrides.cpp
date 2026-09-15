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

    std::optional<rfl::Generic> DiffPayload( const rfl::Generic& base, const rfl::Generic& live,
                                             PrefabDiffReport* report )
    {
        const auto baseObject = base.to_object();
        const auto liveObject = live.to_object();

        // Not both objects: no fields to take apart, so the payload is the unit (see the header).
        if ( !baseObject || !liveObject )
        {
            if ( SameGeneric( base, live ) )
            {
                return std::nullopt;
            }
            return live;
        }

        rfl::Generic::Object differing;
        for ( const auto& [field, value] : liveObject.value() )
        {
            const auto inBase = baseObject.value().get( field );
            if ( !inBase )
            {
                // The file does not state this field at all — see PrefabDiffReport::UnstatedFields for
                // why it is recorded rather than skipped, and why that is the safe direction.
                if ( report != nullptr )
                {
                    ++report->UnstatedFields;
                }
                differing[field] = value;
                continue;
            }
            if ( SameGeneric( inBase.value(), value ) )
            {
                continue;
            }
            differing[field] = value;
        }

        if ( differing.size() == 0 )
        {
            return std::nullopt;
        }
        return { differing };
    }

    rfl::Generic MergePayload( const rfl::Generic& current, const rfl::Generic& partial )
    {
        const auto currentObject = current.to_object();
        const auto partialObject = partial.to_object();

        // Mirror image of DiffPayload's refusal to invent structure: if either side is not an object the
        // override IS the value, because that is the only form the diff could have produced.
        if ( !currentObject || !partialObject )
        {
            return partial;
        }

        rfl::Generic::Object merged = currentObject.value();
        for ( const auto& [field, value] : partialObject.value() )
        {
            merged[field] = value;
        }
        return { merged };
    }

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
            if ( !inBase )
            {
                // A component the prefab does not have at all. There is nothing to merge onto, so the
                // whole payload is the difference — and this is the one case where a component-sized
                // override is still the right answer.
                over.Components[key] = value;
                anything             = true;
                continue;
            }

            // FIELD BY FIELD from here (Ю20). Recording the whole payload because one field moved is
            // what made an overridden instance stop following its prefab for every other field of that
            // component.
            if ( auto fields = DiffPayload( inBase.value(), value, report ) )
            {
                over.Components[key] = std::move( *fields );
                anything             = true;
            }
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

    EntityData OverrideMetaAsEntityData( const PrefabOverrideData& over )
    {
        EntityData data;
        data.Tag         = over.Tag;
        data.Translation = over.Translation;
        data.Rotation    = over.Rotation;
        data.Scale       = over.Scale;
        // AND NOT THE COMPONENTS. They are partial payloads now; the entity deserializer writes a payload
        // onto a component wholesale, so handing it one would reset every field the override does not
        // mention. The caller merges them (MergePayload) against what the entity already holds.
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
            const auto inBase = base.Components.get( key );
            // THE SAME MERGE THE LIVE APPLIER DOES, and the suite holds the two to it. Assigning the
            // partial payload here instead would make a nested prefab's base record lose every field the
            // override does not state — and that base is what the next diff is taken against, so the loss
            // would come back as a fresh "override" on every save.
            base.Components[key] = inBase ? MergePayload( inBase.value(), value ) : value;
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
