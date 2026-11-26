#pragma once

// THE SHAPE OF ONE COMPONENT ROW (ReflectedComponentBlocks.hpp says what a row is and who reads the list).
// Split out of that header so a reader of ONE row -- the generated per-module registration
// (Engine/Generated/Components_<Module>.gen.cpp, ComponentRegistration.hpp) -- does not include the whole
// generated list and with it every component header of every module (BUILD-MOD-5).

namespace Desert::Core::Serialize
{
    // Where a run is registered among the hand-written serializers (see ORDER IS NOT FORMAT above: load order
    // only).
    enum class ReflectedBlockRun
    {
        ActorsAndUI,          // after the animation handlers, before UIRenderTexture
        UIAfterRenderTexture, // after UIRenderTexture, before CubeGridBlockout
        Landscape,            // between the Landscape root and its tiles
        SkyAndAtmosphere      // after the tiles, before Script
    };

    // A block that is one reflected `TData` member of `TComponent`.
    template <class TComponent, class TData>
    struct ReflectedMemberBlock
    {
        using Component = TComponent;
        using Data      = TData;

        const char* Key;      // the block's key in a record
        const char* TypeName; // the reflected type, as ReflectionRegistry names it
        TData TComponent::*Member;
        ReflectedBlockRun  Run;
        unsigned           Version = 1; // COMPONENT( Version( N ) ): the block's stored-shape version
    };

    // A block that is the WHOLE reflected component (Skybox).
    template <class TComponent>
    struct ReflectedWholeBlock
    {
        using Component = TComponent;
        using Data      = TComponent;

        const char*       Key;
        const char*       TypeName;
        ReflectedBlockRun Run;
        unsigned          Version = 1; // COMPONENT( Version( N ) ): the block's stored-shape version
    };
} // namespace Desert::Core::Serialize
