#pragma once

// Annotation macros consumed by DesertHeaderTool (the codegen step). They intentionally expand to
// NOTHING during normal C++ compilation — the tool parses them straight from source and emits the
// registration code into Source/Engine/Generated/Reflection.gen.cpp.
//
// Usage:
//   struct ExampleData
//   {
//       REFLECT()
//
//       PROPERTY( DisplayName( "Albedo" ), Category( "Surface" ), Color )
//       glm::vec4 AlbedoColor;
//
//       PROPERTY( DisplayName( "Metallic" ), Category( "Surface" ), Range( 0.0f, 1.0f ) )
//       float MetallicFactor;
//   };
//
// Supported attribute tokens (parsed by the tool, never compiled):
//   DisplayName("..."), Category("..."), Tooltip("..."), Header("..."), Range(min,max), Color,
//   Asset<TypeName>, Thumbnail, ReadOnly, Hidden, Length, Units("..."), Advanced, Summary,
//   Temperature, Preview, EditCondition("...")
//
//   Length          the number is a world distance, i.e. centimetres (see docs/UNITS.md)
//   Units("deg")    display suffix + a drag step suited to the quantity ("deg", "s", "%", "x", ...)
//   Advanced        folds under an "Advanced" node at the end of its category
//   Summary         feeds the one-line summary beside the component's header, visible while collapsed
//   Temperature     on a Color field: adds a Kelvin slider that writes the RGB (the colour stays the value)
//   Preview         an asset slot shows its content inline instead of only on hover
//   EditCondition("Foo")  grey the row out while the bool field Foo of the same block is false ("!Foo" inverts)
//
// REFLECT() marks a struct/class for reflection. PROPERTY(...) marks the field that follows it.
#define REFLECT()
#define PROPERTY( ... )
