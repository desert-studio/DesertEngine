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
//
// FUNCTION(...) marks the member function (or static function) that follows it, inside a REFLECT() type:
//
//       FUNCTION( ScriptCallable, Category( "Light" ), Tooltip( "Scales the intensity." ) )
//       void ScaleIntensity( float factor );
//
// The tool emits a FunctionInfo whose Invoke calls it through Values (Engine/Reflection/Value.hpp) — the one
// layer any language binds to (FunctionThunk.hpp). Attributes: ScriptCallable, Category("..."), Tooltip("...").
// Every parameter is named; a name is a FUNCTION once per type (no overloads: a caller calls by name).
//
// EVENT(...) marks the event signature that follows it, inside a REFLECT() type — UE's
// DECLARE_DYNAMIC_MULTICAST_SPARSE_DELEGATE (PrimitiveComponent.h OnComponentHit): a named alias of a void function
// type whose parameters are the event's payload:
//
//       EVENT( Category( "Collision" ), Tooltip( "This body struck another." ) )
//       using OnHit = void( entt::entity other, glm::vec3 point, glm::vec3 normal, float impulse );
//
// The tool emits an EventInfo (name, owner, parameters as Value kinds deduced from the alias by MakeEvent in
// FunctionThunk.hpp); a subscriber in any language binds to it by type and name (ECS/ComponentEvents.hpp) and
// receives the payload as Values. Every parameter is named; a name is an EVENT once per type. Attributes:
// Category("..."), Tooltip("...").
//
// COMPONENT(...) marks an ECS component whose whole scene block is its reflection (a struct, not REFLECT()):
//
//       struct CameraComponent
//       {
//           COMPONENT( Key( "Camera" ), Block( Data ), Run( ActorsAndUI ) )
//           CameraData Data;
//       };
//
// Key("...") is the block's key in a scene record; Block( Member ) names the reflected member written as the
// block, or Whole when the component itself is REFLECT() (Skybox); Run( ... ) is the ReflectedBlockRun the
// serializer is registered in. The tool emits Engine/Generated/ReflectedComponentBlocks.gen.hpp from these
// markers - the one list ComponentRegistry, ECS::ReflectedComponents and SceneMigrator read.
#define REFLECT()
#define PROPERTY( ... )
#define FUNCTION( ... )
#define EVENT( ... )
#define COMPONENT( ... )
