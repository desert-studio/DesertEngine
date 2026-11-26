#pragma once

// THE SCENE-SCHEMA LOCK — Desert/Schema/ComponentSchema.lock, written and checked by the header tool on every
// build.
//
// A scene record stores each COMPONENT block (ComponentBlocks.hpp) as its reflected fields by name; a field the
// file lacks keeps the default (ReflectionSerializer). So ADDING a field never breaks a saved scene and needs no
// version bump, while REMOVING, RENAMING or RETYPING one silently drops the stored value. The lock is the shape
// every block was last built with; the build compares the code against it:
//   - a new field or a new block      -> the lock is rewritten, no version moves (the extra v77-style bump is
//   gone);
//   - a field gone at the same version -> a build error naming the two ways out: PROPERTY( WasNamed( "Old" ) ) on
//     the field that replaced it (the loader reads the old key, ReflectionSerializer), or
//     COMPONENT( ..., Version( N+1 ) ) with the block's upgrade step;
//   - a type changed at the same version -> a build error asking for Version( N+1 );
//   - Version( N ) above the locked one -> the block's new shape is locked as is (its upgrade step carries the
//     old data); below it -> an error, versions only rise.
// UE (tagged properties + FCustomVersion + CoreRedirects) and Unity (FormerlySerializedAs) leave the discipline to
// the developer: a forgotten bump or redirect breaks old content silently. Here the generator refuses the build.

#include <string>
#include <utility>
#include <vector>

namespace Desert::HeaderTool
{
    // One reflected field of a block, nested fields included: "Color", "Shape.Radius", "Points[].Position".
    struct SchemaField
    {
        std::string path;
        // What the stored value is: CanonicalSchemaType of the C++ spelling, "{}" for a reflected struct (its
        // fields are rows of their own), "[{}]" for a container of them, "enum <Name>" for an enum.
        std::string                                    type;
        std::vector<std::pair<std::string, long long>> enumerators; // for an enum: every enumerator and its value
        std::vector<std::string>
             wasNamed; // PROPERTY( WasNamed( "Old" ) ): earlier names of the path's last segment
    };

    struct SchemaBlock
    {
        std::string              key;
        unsigned                 version = 1; // COMPONENT( Version( N ) ), 1 when not stated
        std::string              where;       // the COMPONENT marker's file:line (the code side only)
        std::vector<SchemaField> fields;      // declaration order, a struct's fields right after the struct
    };

    // The spelling with its whitespace and namespace qualifiers removed: "std::vector< Desert::Foo >" ->
    // "vector<Foo>". A type that moves between namespaces is the same stored value.
    std::string CanonicalSchemaType( const std::string& cppSpelling );

    // The lock's text: a comment header, then per block (ordered by key) "[Key] Version N" and one
    // "path: type" line per field. Deterministic, so the committed file only changes with the schema.
    std::string WriteSchemaLock( const std::vector<SchemaBlock>& blocks );

    // The lock read back; a malformed line is an error naming its line.
    std::vector<SchemaBlock> ReadSchemaLock( const std::string& text, std::vector<std::string>& errors );

    struct SchemaLockVerdict
    {
        std::vector<std::string> errors;  // each one says what to write and where
        std::vector<std::string> changes; // what the rewritten lock adds, renames or re-versions
    };

    // `locked` (the committed lock) against `current` (the code). Without errors the lock becomes `current`.
    SchemaLockVerdict CheckSchemaLock( const std::vector<SchemaBlock>& locked,
                                       const std::vector<SchemaBlock>& current );
} // namespace Desert::HeaderTool
