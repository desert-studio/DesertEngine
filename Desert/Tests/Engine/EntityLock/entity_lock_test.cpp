// У5-2 — the authoring lock, as a rule rather than as three opinions.
//
// The lock is asked about from three unrelated places: the viewport's picker refuses to select a locked
// entity, the gizmo refuses to draw handles for one, and the outliner draws the padlock that says so. A
// lock whose three readers disagree is WORSE than no lock, because the closed padlock keeps promising
// something one of the other two is not doing.
//
// So there is one predicate (ECS::IsLocked) and one mutator (ECS::SetLockedRecursive) and this suite is
// about them. The relation that carries the feature is the RECURSIVE one: after locking a root, NO
// descendant anywhere below it may answer "not locked" — because the thing a user locks is a prop, and
// the thing they then accidentally grab is one of its meshes.

#include <gtest/gtest.h>

#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EntityLock.hpp>

#include <entt/entt.hpp>

#include <vector>

using Desert::ECS::IsLocked;
using Desert::ECS::LockComponent;
using Desert::ECS::RelationshipComponent;
using Desert::ECS::SetLockedRecursive;

namespace
{
    // A small hierarchy built by hand, so the suite needs no Scene:
    //
    //     root
    //      +- childA
    //      |   +- grandchild
    //      +- childB
    //
    // ...plus `outsider`, an unrelated root that must never be touched by an operation on `root`.
    struct Fixture
    {
        entt::registry Registry;
        entt::entity   Root{ entt::null };
        entt::entity   ChildA{ entt::null };
        entt::entity   ChildB{ entt::null };
        entt::entity   Grandchild{ entt::null };
        entt::entity   Outsider{ entt::null };

        Fixture()
        {
            Root       = Make();
            ChildA     = Make();
            ChildB     = Make();
            Grandchild = Make();
            Outsider   = Make();

            Attach( Root, ChildA );
            Attach( Root, ChildB );
            Attach( ChildA, Grandchild );
        }

        std::vector<entt::entity> Subtree() const
        {
            return { Root, ChildA, ChildB, Grandchild };
        }

    private:
        entt::entity Make()
        {
            const auto e = Registry.create();
            Registry.emplace<RelationshipComponent>( e );
            return e;
        }

        void Attach( entt::entity parent, entt::entity child )
        {
            Registry.get<RelationshipComponent>( parent ).Children.push_back( child );
            Registry.get<RelationshipComponent>( child ).Parent = parent;
        }
    };
} // namespace

// The default has to be UNLOCKED, and it has to come from the component being ABSENT rather than from a
// field being false. Every scene written before this component existed loads without the key, and those
// entities must be editable -- a migration that silently froze an old level would be the worst possible
// reading of "the lock persists".
TEST( EntityLock, AnEntityWithNoLockComponentIsEditable )
{
    Fixture f;
    for ( const auto e : f.Subtree() )
        EXPECT_FALSE( IsLocked( f.Registry, e ) );
}

// THE LOAD-BEARING RELATION. Not "the root is locked" -- that is a spot value and would pass on a setter
// that forgot to recurse at all. The claim is about the whole subtree at once: nothing below a locked
// root escapes, at any depth.
TEST( EntityLock, LockingARootLeavesNoDescendantEditable )
{
    Fixture f;
    SetLockedRecursive( f.Registry, f.Root, true );

    for ( const auto e : f.Subtree() )
        EXPECT_TRUE( IsLocked( f.Registry, e ) ) << "an entity in the locked subtree is still editable";

    // ...and the relation has a second half, or "lock everything in the scene" would satisfy it.
    EXPECT_FALSE( IsLocked( f.Registry, f.Outsider ) ) << "an unrelated root was locked as collateral";
}

TEST( EntityLock, UnlockingARootFreesTheWholeSubtree )
{
    Fixture f;
    SetLockedRecursive( f.Registry, f.Root, true );
    SetLockedRecursive( f.Registry, f.Root, false );

    for ( const auto e : f.Subtree() )
        EXPECT_FALSE( IsLocked( f.Registry, e ) ) << "an entity stayed locked after the subtree was unlocked";
}

// The lock travels DOWN the hierarchy, never up. Locking one mesh of a prop must not freeze the prop, or
// a single mis-click in the outliner would take the whole thing out of reach with no obvious way back.
TEST( EntityLock, LockingAChildDoesNotLockItsAncestors )
{
    Fixture f;
    SetLockedRecursive( f.Registry, f.ChildA, true );

    EXPECT_TRUE( IsLocked( f.Registry, f.ChildA ) );
    EXPECT_TRUE( IsLocked( f.Registry, f.Grandchild ) ) << "the child's own subtree must follow it";

    EXPECT_FALSE( IsLocked( f.Registry, f.Root ) ) << "the lock climbed to the parent";
    EXPECT_FALSE( IsLocked( f.Registry, f.ChildB ) ) << "the lock reached a sibling";
}

// Both directions have to be repeatable. A marker component emplaced twice is an entt assertion in a
// debug build, and a removal of something absent is another -- both reachable by a user who clicks the
// padlock on an already-locked row, or locks a selection whose members overlap.
TEST( EntityLock, SettingTheSameStateTwiceIsHarmless )
{
    Fixture f;

    SetLockedRecursive( f.Registry, f.Root, true );
    SetLockedRecursive( f.Registry, f.Root, true );
    for ( const auto e : f.Subtree() )
        EXPECT_TRUE( IsLocked( f.Registry, e ) );

    SetLockedRecursive( f.Registry, f.Root, false );
    SetLockedRecursive( f.Registry, f.Root, false );
    for ( const auto e : f.Subtree() )
        EXPECT_FALSE( IsLocked( f.Registry, e ) );
}

// Overlapping operations: locking a child and THEN its parent, then unlocking the parent, leaves nothing
// locked. This is the known cost of stamping a subtree rather than inheriting it -- the same cost
// Scene::SetVisibleRecursive already pays for visibility -- and it is written down as a test so it is a
// decision on the record rather than a surprise found later.
TEST( EntityLock, UnlockingAParentAlsoFreesAChildThatWasLockedOnItsOwn )
{
    Fixture f;
    SetLockedRecursive( f.Registry, f.ChildA, true );
    SetLockedRecursive( f.Registry, f.Root, true );
    SetLockedRecursive( f.Registry, f.Root, false );

    EXPECT_FALSE( IsLocked( f.Registry, f.ChildA ) )
         << "the subtree is STAMPED, not inherited: an individually locked child is cleared by unlocking "
            "an ancestor, exactly as visibility behaves";
}

// An entity with no RelationshipComponent at all is the common case in a flat scene, and the recursion
// must simply stop rather than reach for a component that is not there.
TEST( EntityLock, AChildlessEntityWithNoRelationshipComponentLocksFine )
{
    entt::registry registry;
    const auto     lonely = registry.create();

    SetLockedRecursive( registry, lonely, true );
    EXPECT_TRUE( IsLocked( registry, lonely ) );

    SetLockedRecursive( registry, lonely, false );
    EXPECT_FALSE( IsLocked( registry, lonely ) );
}

// entt::null reaches these functions from any call site whose lookup missed. Answering "not locked" and
// doing nothing is the safe pair; asking the registry about a null entity is undefined behaviour.
TEST( EntityLock, TheNullEntityIsNeitherLockedNorLockable )
{
    entt::registry registry;

    EXPECT_FALSE( IsLocked( registry, entt::null ) );
    SetLockedRecursive( registry, entt::null, true ); // must not reach the registry at all
    EXPECT_FALSE( IsLocked( registry, entt::null ) );
}
