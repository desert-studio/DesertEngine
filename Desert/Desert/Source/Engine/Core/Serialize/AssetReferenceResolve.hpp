#pragma once

namespace Desert::Core::Serialize
{
    /// HOW A SCENE'S REFERENCE REACHED THE RECORD IT NAMES.
    ///
    /// It exists so that the two routes can be TOLD APART BY A TEST, not so that they can be treated
    /// differently — see ResolveSceneReference below, whose whole claim is that they are not.
    enum class ReferenceOrigin
    {
        Found,  ///< the registry already held a record for this file (preloader, another scene, an open document)
        Created ///< this parse minted the record
    };

    /// THE RELATION THIS HEADER EXISTS TO STATE, and the reason it is a function rather than a comment:
    ///
    ///     EVERY ASSET A LOADED SCENE REFERENCES IS REGISTERED IN ITS SERVICE, ON EVERY ROUTE THAT
    ///     RESOLVES THE REFERENCE — whether the record was FOUND or CREATED.
    ///
    /// Both halves of the old code were individually correct. `FindByPath` returning an existing record is
    /// a correct lookup; registering a record you have just created is a correct registration. The
    /// disagreement was that only the CREATED route registered, so a reference to an asset somebody else
    /// had already created resolved to a live handle that no service could answer for. Nothing was ever
    /// seen to break, because a boot scanner (since deleted) registered every mesh and material before any scene
    /// is allowed to load (`EditorLayer::OnUpdate`: "Scene loads wait until the startup stages finished").
    /// THAT IS A SAFETY NET AND NOT A GUARANTEE: it is stated nowhere the parse can read, it covers only
    /// the two content roots the preloader walks, and it disappears with any refactor by somebody who does
    /// not know it is load-bearing. Four defects in this tree were one disagreement found four times
    /// because each side was asserted separately; this is the shape that closes the class.
    ///
    /// @param find             -> a record, or a falsy record when the registry does not hold this file.
    /// @param create           -> a record, or a falsy record when the file cannot become one.
    /// @param ensureRegistered ( record, origin ) -> void. Called EXACTLY ONCE, for BOTH origins, and
    ///                         never for a reference that resolved to nothing. Idempotent by contract:
    ///                         the registration a service already holds must cost a lookup and no work,
    ///                         because the Found route is the common one and it runs per reference per
    ///                         scene load.
    ///
    /// @return the resolved record, falsy when the reference names nothing.
    template <class TFind, class TCreate, class TEnsure>
    auto ResolveSceneReference( TFind&& find, TCreate&& create, TEnsure&& ensureRegistered )
    {
        auto            record = find();
        ReferenceOrigin origin = ReferenceOrigin::Found;
        if ( !record )
        {
            record = create();
            origin = ReferenceOrigin::Created;
        }
        // A reference that names nothing is NOT registered and NOT reported here: the caller knows which
        // asset type and which spelling failed, and it is the caller that owes the message (DC 1.4).
        if ( !record )
            return record;

        ensureRegistered( record, origin );
        return record;
    }
} // namespace Desert::Core::Serialize
