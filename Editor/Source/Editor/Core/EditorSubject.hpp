#pragma once

// DELIBERATELY FREE OF THE EDITOR AND THE ENGINE. This header is included by IPanel.hpp, which is included
// by EditorLayer.hpp ahead of the render-system headers; anything it drags in is dragged in there too, and
// SubjectEditorRegistry.hpp's own top note records what that cost the last time (a header opening
// `Desert::Editor::Core` silently rebound every unqualified `Core::Scene` after it). It is also what makes
// the seam testable at all: the suites that assert these rules link `Common` and nothing else.
#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/UUID.hpp>

#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <string_view>

namespace Desert::Editor
{
    // ── WHAT A DOCUMENT EDITS ──────────────────────────────────────────────────────────────────────────
    //
    // The seam used to be keyed on an ASSET HANDLE, and every document in the editor was therefore an
    // asset. That was true of the five that existed and false of the ones the owner asked for next: an
    // anim graph, a particle emitter and a UI canvas are authored data that lives in a COMPONENT ON AN
    // ENTITY and has no file of its own. The button the owner wanted in the Details panel — "edit this
    // thing" beside the component that holds it — had nowhere to go, because the request that button
    // sends carries an `AssetHandle` and a component is not one.
    //
    // SO THE KEY IS A SUBJECT, AND A SUBJECT IS NOT A UNION OF TWO CASES. Writing it as
    // `variant<AssetHandle, pair<EntityUUID, ComponentKind>>` would have been the two cases that exist
    // today frozen into the type, and the third one is already visible: one emitter inside a system, one
    // state inside a graph — an owner plus a part of it. What a subject actually is, is three things:
    //
    //   IDENTITY   this and no other, stable across frames, comparable, printable
    //   REACH      a way to get from the identity back to the data (who knows how, and in what)
    //   LIFE       a way to know the data is still there
    //
    // This header carries the IDENTITY. REACH is the factory registered for the subject's type
    // (SubjectEditorRegistry::Registration::Make) — it is typed, so it cannot live in a common struct.
    //
    // LIFE IS ASKED TWICE, of two different things, and that is not a duplication:
    //
    //   ISubjectDocument::IsSubjectAlive   "is what THIS WINDOW is bound to still there?" — asked of the
    //                                      document, because the document holds the context it was opened
    //                                      in. An anim graph opened over the second scene view is alive or
    //                                      dead according to THAT scene, not the active one.
    //   Registration::Exists               "is there anything under this subject to open?" — asked of the
    //                                      registry, about subjects that are NOT open, in the CURRENT
    //                                      context. This is what the command palette enumerates with, and
    //                                      no document can answer it because none exists yet.
    //
    // Neither can stand in for the other; the two comment blocks at those declarations say why at length.
    //
    // Identity decomposes into three fields and not one, and none of the three is spare:
    //
    //   DOMAIN   what kind of thing OWNS the data: a file, an entity. The registry is keyed on
    //            (domain, facet), so a component type and an asset type can never collide even if their
    //            numbers agree.
    //   FACET    WHICH KIND within that domain: the AssetTypeID for a file, the component's type for an
    //            entity. This is the existing `AssetOpenRequest::Type` field generalised — the editor
    //            already had to carry it, because a handle alone never said which editor to open.
    //   OWNER    the 64-bit id of the owner: the AssetHandle, or the entity's UUID.
    //
    // WHAT IS DELIBERATELY ABSENT: an ordinal saying WHICH of several same-kind parts of one owner. The
    // two future cases above will need one, and nothing today writes one — a field no caller sets is a
    // dead parameter, and this file would be asserting an identity it cannot tell apart. When the first
    // "one emitter of several" document arrives it adds a fourth field here and the census in
    // SubjectEditorRegistry is what will show every call site that has to learn about it.
    enum class SubjectDomain : uint8_t
    {
        Unknown = 0,

        // A FILE in the content tree. Owner is its Assets::AssetHandle, Facet its Assets::AssetTypeID.
        Asset,

        // AUTHORED DATA HELD BY A COMPONENT ON AN ENTITY. Owner is the entity's Common::UUID (the id the
        // scene serializes and SelectionManager already speaks — Engine/ECS/Components.hpp UUIDComponent),
        // Facet the component type, via ComponentFacet below.
        EntityComponent,

        // NOT a domain: the number of them. New domains go ABOVE this line; SubjectDomainName's switch has
        // no `default:`, so -Wswitch reports the omission at the point of it.
        Count,
    };

    // The domain's name, for logs, window ids and the control channel. No `default:` label, for the reason
    // Assets::AssetTypeName gives at its own definition: a name table that quietly falls behind its enum
    // puts the wrong word in the one message whose whole job is to be trusted. The trailing return is
    // reached only by a value no enumerator names.
    constexpr const char* SubjectDomainName( const SubjectDomain domain ) noexcept
    {
        switch ( domain )
        {
            case SubjectDomain::Unknown:
                return "unknown";
            case SubjectDomain::Asset:
                return "asset";
            case SubjectDomain::EntityComponent:
                return "component";
            case SubjectDomain::Count:
                return "count";
        }
        return "unknown";
    }

    // THE FACET OF A COMPONENT SUBJECT: an FNV-1a digest of the component's own C++ type name.
    //
    // Derived and not enumerated, deliberately. An enum of component kinds would be a second census beside
    // the ECS component list, kept by hand, and the failure mode when it fell behind would be a Details
    // button that opens the wrong editor. Deriving it from the type's name means the identity IS the type:
    // there is nothing to keep in step.
    //
    // A DIGEST CAN COLLIDE, AND THE COLLISION IS CAUGHT RATHER THAN SUFFERED. Two component names landing
    // on one 32-bit value would silently share an editor; SubjectEditorRegistry::Register refuses a second
    // registration under a key it already holds under a DIFFERENT name and says both names, so a collision
    // is a startup error naming the two types rather than a wrong window some weeks later.
    [[nodiscard]] constexpr uint32_t ComponentFacet( const std::string_view componentTypeName ) noexcept
    {
        uint32_t hash = 2166136261u;
        for ( const char c : componentTypeName )
        {
            hash ^= static_cast<uint32_t>( static_cast<unsigned char>( c ) );
            hash *= 16777619u;
        }
        // Zero is "no facet" — an unset SubjectId must never be mistaken for a real component type.
        return hash != 0u ? hash : 1u;
    }

    // WHICH KIND OF SUBJECT — the registry's key, and the only thing that decides which editor opens.
    //
    // A pair rather than a single number because the two halves come from different authorities: the
    // domain is this header's enum, the facet is an asset type or a type-name digest. Flattening them
    // would make `AssetTypeID::Material == 2` and a component whose digest happens to be 2 the same key.
    struct SubjectTypeKey
    {
        SubjectDomain Domain = SubjectDomain::Unknown;
        uint32_t      Facet  = 0;

        [[nodiscard]] friend bool operator==( const SubjectTypeKey& lhs, const SubjectTypeKey& rhs ) noexcept
        {
            return lhs.Domain == rhs.Domain && lhs.Facet == rhs.Facet;
        }
    };

    // ONE SUBJECT: a kind, and which one of that kind.
    //
    // A value type with no behaviour beyond identity, so it can be a map key, a queued request, a window
    // id and a line on the control channel without any of those needing the editor.
    struct SubjectId
    {
        SubjectDomain Domain = SubjectDomain::Unknown;
        uint32_t      Facet  = 0;
        Common::UUID  Owner;

        [[nodiscard]] SubjectTypeKey Type() const noexcept
        {
            return SubjectTypeKey{ Domain, Facet };
        }

        // NOTHING. A default-constructed SubjectId, and what Find/Release answer about.
        //
        // All three parts have to be set for a subject to name anything: a domain with no owner is a kind
        // with no instance, and an owner with no domain does not say what the number means. The null
        // handle is "no asset" and the null UUID is "no entity", which is the same test on the same field.
        [[nodiscard]] bool IsNull() const noexcept
        {
            return Domain == SubjectDomain::Unknown || Facet == 0u || Owner.IsNull();
        }

        [[nodiscard]] friend bool operator==( const SubjectId& lhs, const SubjectId& rhs ) noexcept
        {
            return lhs.Domain == rhs.Domain && lhs.Facet == rhs.Facet &&
                   static_cast<uint64_t>( lhs.Owner ) == static_cast<uint64_t>( rhs.Owner );
        }

        // THE CANONICAL TEXT FORM: "<domain>:<facet>:<owner>", e.g. "asset:2:333333" or
        // "component:1729384756:88".
        //
        // ALL THREE PARTS, AND NOT A HASH OF THEM. This string is the ImGui window id (see
        // DocumentTitle in IPanel.hpp) and the `subject` field the control channel reports, and both of
        // those are places where a collision is silent: two windows whose ids agree are ONE window in
        // ImGui, merged, with the second one's content drawn into the first. A digest would make that
        // possible for no gain — the string is built once per frame per document, and it is short.
        [[nodiscard]] std::string ToString() const
        {
            return std::format( "{}:{}:{}", SubjectDomainName( Domain ), Facet, static_cast<uint64_t>( Owner ) );
        }
    };

    // A FILE. @p type is the Assets::AssetTypeID; taken as a uint32 so this header stays free of
    // Engine/Assets/Common.hpp — see the top note about what IPanel.hpp drags along.
    [[nodiscard]] inline SubjectId AssetSubject( const Common::AssetHandle& handle, const uint32_t type ) noexcept
    {
        return SubjectId{ SubjectDomain::Asset, type, handle };
    }

    // AUTHORED DATA IN A COMPONENT. @p componentTypeName is the component's own C++ type name; pass it as
    // a literal at the call site so the identity is visible there rather than hidden behind a constant.
    [[nodiscard]] inline SubjectId ComponentSubject( const Common::UUID&    entity,
                                                     const std::string_view componentTypeName ) noexcept
    {
        return SubjectId{ SubjectDomain::EntityComponent, ComponentFacet( componentTypeName ), entity };
    }

    // The type key for a component kind, WITHOUT an instance — what a factory is registered under.
    [[nodiscard]] constexpr SubjectTypeKey
    ComponentSubjectType( const std::string_view componentTypeName ) noexcept
    {
        return SubjectTypeKey{ SubjectDomain::EntityComponent, ComponentFacet( componentTypeName ) };
    }

    // The type key for an asset kind, WITHOUT an instance.
    [[nodiscard]] constexpr SubjectTypeKey AssetSubjectType( const uint32_t type ) noexcept
    {
        return SubjectTypeKey{ SubjectDomain::Asset, type };
    }
} // namespace Desert::Editor

namespace std
{
    template <>
    struct hash<Desert::Editor::SubjectTypeKey>
    {
        std::size_t operator()( const Desert::Editor::SubjectTypeKey& key ) const noexcept
        {
            return ( static_cast<std::size_t>( key.Facet ) << 8 ) ^ static_cast<std::size_t>( key.Domain );
        }
    };

    template <>
    struct hash<Desert::Editor::SubjectId>
    {
        std::size_t operator()( const Desert::Editor::SubjectId& subject ) const noexcept
        {
            const std::size_t owner = static_cast<std::size_t>( static_cast<uint64_t>( subject.Owner ) );
            return owner ^ ( hash<Desert::Editor::SubjectTypeKey>{}( subject.Type() ) * 1099511628211ull );
        }
    };
} // namespace std
