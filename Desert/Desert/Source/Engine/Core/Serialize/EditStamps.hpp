#pragma once

// WHAT THE UNDO HISTORY SEES OF A SCENE'S ENTITY PACKAGES (WP17).
//
// The editor's CommandHistory stamps the entities each record changed and puts the stamps back on undo/redo
// (UE's transaction remembering each package's dirty state). It needs those three operations and nothing else
// of the ledger, and it is a header-only class read by suites that link no engine code - so it holds this
// interface, and EntityPackages (EntityPackages.hpp) is the one implementation.

#include <Common/Core/UUID.hpp>

#include <cstdint>

namespace Desert::Core
{
    class IEditStamps
    {
    public:
        using Revision = std::uint64_t;

        struct Stamp
        {
            Common::UUID Id;
            Revision     Before = 0;
            Revision     After  = 0;
        };

        // An edit changed entity `id`: a fresh revision, and the one it replaces (for the undo record).
        virtual Stamp Touch( Common::UUID id ) = 0;
        // Undo/redo: entity `id` is back at revision `revision`.
        virtual void Restore( Common::UUID id, Revision revision ) = 0;
        // An edit whose entities were not named: the next save is whole.
        virtual void TouchAll() = 0;

    protected:
        IEditStamps()                                = default;
        IEditStamps( const IEditStamps& )            = default;
        IEditStamps& operator=( const IEditStamps& ) = default;
        ~IEditStamps()                               = default;
    };
} // namespace Desert::Core
