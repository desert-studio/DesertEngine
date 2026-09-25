// THE SCENE SHOWS THE APPLIED STATE, THE PREVIEW SHOWS THE WORKING COPY, AND THE TWO COINCIDE EXACTLY
// AFTER APPLY.
//
// That is ONE claim seen from three sides, and it is a claim about a RELATION between states rather than
// about any function's return value. Before this the editor had one state: an edit went into the asset
// every scene renders from, in the same statement that pushed it at the runtime, so a slider moved every
// mesh in the level immediately and nothing anywhere could put it back. Each half of that was individually
// correct code — which is exactly why the defect was invisible to a unit test of either half.
//
// The three sides:
//
//   1. THE VALUE LOGIC. Three MaterialData play the three states, and the whole life of a document is
//      walked through them: open, edit, apply, edit, discard, save. Both "dirty"s are asserted at every
//      step, in both directions, because the two are different questions (the scene vs the file) and
//      collapsing them was the reason task U6 could not put a dot on a document tab.
//
//   2. THE IDENTITY. The working copy is a second material asset with a header GUID of its own, so the
//      transfer between states must move the AUTHORED half and nothing else. A whole-struct assignment
//      would hand one of the two ids to the other, and MaterialService keys the mesh -> material link on
//      exactly that id: whichever registered first would then answer for both, and a mesh in the level
//      would start drawing the preview's material.
//
//   3. WHICH MATERIAL EACH AUDIENCE IS HANDED, and that a value reaches it through ONE WRITE, read out of
//      MaterialEditorPanel.cpp. The panel owns a PreviewViewport, which owns a Scene and a SceneRenderer,
//      and neither can be constructed without a Vulkan device -- so this side is a source-level assertion,
//      the established alternative here (Tests/Editor/MaterialPreviewRoute and Tests/Engine/
//      SettingConsumers do the same thing and say so). It fails in BOTH directions on purpose: someone who
//      re-merges the two wires "because the preview and the scene must agree" has to be stopped and told
//      that agreement comes from the shared working copy, not from the two audiences being one object.
//
//   4. WHAT THE CONTROL CHANNEL MAY WRITE, which is the shader's own schema and never a list beside it.
//      The channel can now edit the focused document's properties -- the half of "everything a person can
//      do" that has no name and therefore cannot be a palette command. A hand-written census would be a
//      THIRD list of parameter names beside the schema and the window's table; this side is what makes
//      the derivation itself go red.

#include <gtest/gtest.h>

#include <Editor/Panels/MaterialEditor/MaterialEditStates.hpp>

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using Desert::Assets::MaterialData;
namespace MaterialEdit = Desert::Editor::MaterialEdit;

namespace
{
    MaterialData Authored( const char* shader, std::initializer_list<std::pair<const char*, float>> params )
    {
        MaterialData data;
        data.ShaderName = shader;
        for ( const auto& [name, value] : params )
            data.SetParam( name, glm::vec4( value, 0.0f, 0.0f, 0.0f ) );
        return data;
    }

    // The repository root, found by walking up from wherever the binary was started -- the same approach
    // MaterialPreviewRoute and SettingConsumers use, so none of them needs one exact directory.
    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Editor/Source/Editor/Panels/MaterialEditor/MaterialEditorPanel.cpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    // Source with // line comments stripped. This file's subject is discussed at length in the panel's own
    // comments, so a sentence that merely NAMES the old one-wire behaviour must not read as code doing it.
    std::string CodeOnly( const std::string& path )
    {
        std::ifstream in( path );
        if ( !in )
            return {};

        std::ostringstream out;
        std::string        line;
        while ( std::getline( in, line ) )
        {
            const std::size_t comment = line.find( "//" );
            out << ( comment == std::string::npos ? line : line.substr( 0, comment ) ) << '\n';
        }
        return out.str();
    }

    std::size_t CountOf( const std::string& haystack, const std::string& needle )
    {
        std::size_t count = 0;
        for ( std::size_t at = haystack.find( needle ); at != std::string::npos;
              at             = haystack.find( needle, at + needle.size() ) )
            ++count;
        return count;
    }

    // The name of the MaterialEditorPanel member function whose body contains @p offset. Definitions in
    // this file are all written "Type MaterialEditorPanel::Name(", so the nearest such marker above an
    // offset is the function it sits in.
    std::string EnclosingFunction( const std::string& code, std::size_t offset )
    {
        const std::string marker = "MaterialEditorPanel::";
        const std::size_t at     = code.rfind( marker, offset );
        if ( at == std::string::npos )
            return {};

        const std::size_t nameStart = at + marker.size();
        const std::size_t paren     = code.find( '(', nameStart );
        if ( paren == std::string::npos )
            return {};
        return code.substr( nameStart, paren - nameStart );
    }
} // namespace

// ── 1. The value logic: one document's whole life, both "dirty"s at every step ──────────────────────────

TEST( MaterialEditStates, ThreeStatesWalkedEndToEnd )
{
    // OPEN. The file was just read into the subject and copied into a working copy, so all three agree and
    // the document is clean in both senses.
    MaterialData onDisk  = Authored( "StaticMeshPBR", { { "RoughnessFactor", 0.9f } } );
    MaterialData applied = onDisk;
    MaterialData working = onDisk;

    auto dirty = MaterialEdit::EvaluateDirty( working, applied, onDisk );
    EXPECT_FALSE( dirty.Unapplied );
    EXPECT_FALSE( dirty.Unsaved );

    // EDIT. The slider moved. The preview reads `working` and has changed; the SCENE reads `applied` and
    // has not -- which is the defect this whole task is about, asserted as a state rather than as a wish.
    working.SetParam( "RoughnessFactor", glm::vec4( 0.1f, 0.0f, 0.0f, 0.0f ) );

    dirty = MaterialEdit::EvaluateDirty( working, applied, onDisk );
    EXPECT_TRUE( dirty.Unapplied ) << "the scene is not showing this edit, so Apply must be offered";
    EXPECT_TRUE( dirty.Unsaved ) << "an edit that has not even been applied is still not in the file";
    EXPECT_EQ( applied.GetFloat( "RoughnessFactor" ), 0.9f ) << "editing must not touch the applied state";

    // APPLY. The scene changes HERE and nowhere else.
    MaterialEdit::CopyAuthoredValues( applied, working );

    dirty = MaterialEdit::EvaluateDirty( working, applied, onDisk );
    EXPECT_FALSE( dirty.Unapplied );
    EXPECT_TRUE( dirty.Unsaved ) << "applied is not saved";
    EXPECT_EQ( applied.GetFloat( "RoughnessFactor" ), 0.1f );

    // EDIT AGAIN, THEN DISCARD -- the step that could not be performed at all before this task.
    working.SetParam( "RoughnessFactor", glm::vec4( 0.42f, 0.0f, 0.0f, 0.0f ) );
    EXPECT_TRUE( MaterialEdit::EvaluateDirty( working, applied, onDisk ).Unapplied );

    MaterialEdit::CopyAuthoredValues( working, applied );

    dirty = MaterialEdit::EvaluateDirty( working, applied, onDisk );
    EXPECT_FALSE( dirty.Unapplied ) << "Discard puts the working copy back to what the scene is showing";
    EXPECT_EQ( working.GetFloat( "RoughnessFactor" ), 0.1f ) << "and the ball goes back with it";
    EXPECT_TRUE( dirty.Unsaved ) << "the applied value is still not in the file";

    // SAVE. Applies first (already equal here), writes, and the snapshot moves.
    MaterialEdit::CopyAuthoredValues( onDisk, working );

    dirty = MaterialEdit::EvaluateDirty( working, applied, onDisk );
    EXPECT_FALSE( dirty.Unapplied );
    EXPECT_FALSE( dirty.Unsaved );
}

TEST( MaterialEditStates, DiscardRestoresEveryKindOfEdit )
{
    // Not only a slider: the shader, a texture binding and a parameter that did not exist before the edit
    // are all things this window can change, and a Discard that restored two of the three would be the
    // "middle link drops a property" shape -- both ends looking right with one field silently lost.
    MaterialData applied = Authored( "StaticMeshPBR", { { "RoughnessFactor", 0.9f } } );
    applied.SetTexture( "u_AlbedoTexture", Common::Content::AssetGuid{ 0, 1234u }, "" );

    MaterialData working = applied;
    working.ShaderName   = "SomeGraphShader";
    working.SetParam( "RoughnessFactor", glm::vec4( 0.0f ) );
    working.SetParam( "MetallicFactor", glm::vec4( 1.0f, 0.0f, 0.0f, 0.0f ) ); // a NEW row
    working.SetTexture( "u_AlbedoTexture", Common::Content::AssetGuid{ 0, 5678u }, "" );
    working.SetTexture( "u_NormalTexture", Common::Content::AssetGuid{ 0, 9999u }, "" ); // a NEW binding

    EXPECT_TRUE( MaterialEdit::EvaluateDirty( working, applied, applied ).Unapplied );

    MaterialEdit::CopyAuthoredValues( working, applied );

    EXPECT_FALSE( MaterialEdit::EvaluateDirty( working, applied, applied ).Unapplied );
    EXPECT_EQ( working.EffectiveShaderName(), "StaticMeshPBR" );
    EXPECT_EQ( working.Params.size(), applied.Params.size() ) << "the added row must be gone, not zeroed";
    EXPECT_EQ( working.GetTexture( "u_AlbedoTexture" ),
               static_cast<uint64_t>( MaterialData::HandleOf( Common::Content::AssetGuid{ 0, 1234u } ) ) );
    EXPECT_EQ( working.GetTexture( "u_NormalTexture" ),
               static_cast<uint64_t>( MaterialData::HandleOf( Common::Content::AssetGuid{ 0, 0u } ) ) )
         << "the added binding must be gone";
}

// ── 2. The identity half, which the transfer must never move ────────────────────────────────────────────

TEST( MaterialEditStates, TransferMovesValuesAndNeverIdentity )
{
    namespace Content = Common::Content;
    const Content::AssetGuid subjectGuid{ 0x111ull, 0x1ull };
    const Content::AssetGuid copyGuid{ 0x222ull, 0x2ull };
    const Content::AssetGuid parentGuid{ 0x777ull, 0x7ull };

    MaterialData subject = Authored( "StaticMeshPBR", { { "RoughnessFactor", 0.9f } } );
    subject.Header       = Content::MakeTextHeader( Content::ContentKind::Material, subjectGuid, {} );
    subject.SetParent( parentGuid );

    MaterialData copy = subject;
    copy.Header->Guid = Content::AssetGuidToText( copyGuid ); // CreateWorkingCopy mints a fresh one
    copy.SetParam( "RoughnessFactor", glm::vec4( 0.2f, 0.0f, 0.0f, 0.0f ) );

    // Apply: values from the copy into the subject, identity untouched in both.
    MaterialEdit::CopyAuthoredValues( subject, copy );
    EXPECT_EQ( subject.GetFloat( "RoughnessFactor" ), 0.2f );
    EXPECT_EQ( subject.Guid(), subjectGuid )
         << "Apply must not hand the working copy's GUID to the subject: two materials claiming one "
            "handle makes the mesh -> material link resolve to whichever registered first";
    EXPECT_EQ( copy.Guid(), copyGuid );
    EXPECT_EQ( subject.ParentGuid(), parentGuid )
         << "this window cannot re-parent a material, so a transfer of the parent could only be an accident";

    // Discard: the other direction, same rule.
    subject.SetParam( "RoughnessFactor", glm::vec4( 0.7f, 0.0f, 0.0f, 0.0f ) );
    MaterialEdit::CopyAuthoredValues( copy, subject );
    EXPECT_EQ( copy.GetFloat( "RoughnessFactor" ), 0.7f );
    EXPECT_EQ( copy.Guid(), copyGuid );
    EXPECT_EQ( subject.Guid(), subjectGuid );
}

TEST( MaterialEditStates, EqualityIsAboutTheValuesAndNotTheirOrder )
{
    MaterialData a;
    a.SetParam( "A", glm::vec4( 1.0f ) );
    a.SetParam( "B", glm::vec4( 2.0f ) );

    MaterialData b;
    b.SetParam( "B", glm::vec4( 2.0f ) );
    b.SetParam( "A", glm::vec4( 1.0f ) );

    EXPECT_TRUE( MaterialEdit::AuthoredValuesEqual( a, b ) )
         << "MaterialData stores parameters in first-written order; comparing positionally would report a "
            "document as permanently unapplied after a Discard";

    // An absent shader name IS "StaticMeshPBR" -- a .demat saved before the field existed must not read as
    // differing from the one the editor just wrote.
    MaterialData unnamed;
    MaterialData named;
    named.ShaderName = "StaticMeshPBR";
    EXPECT_TRUE( MaterialEdit::AuthoredValuesEqual( unnamed, named ) );

    named.ShaderName = "SkinnedMeshPBR";
    EXPECT_FALSE( MaterialEdit::AuthoredValuesEqual( unnamed, named ) );

    // And a difference of one component of one parameter is a difference.
    MaterialData c = a;
    c.SetParam( "B", glm::vec4( 2.0f, 0.0f, 0.0f, 1.0f ) );
    EXPECT_FALSE( MaterialEdit::AuthoredValuesEqual( a, c ) );
}

// ── 3. Which material each audience is handed ───────────────────────────────────────────────────────────

TEST( MaterialEditStates, PreviewIsHandedTheWorkingCopyAndTheSceneIsNot )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "could not locate the repository root from the working directory";

    const std::string code =
         CodeOnly( root + "Editor/Source/Editor/Panels/MaterialEditor/MaterialEditorPanel.cpp" );
    ASSERT_FALSE( code.empty() ) << "MaterialEditorPanel.cpp could not be read";

    // The pane draws the WORKING copy. `SetMaterial( Subject(), ... )` is the old wire, and it is what put
    // the preview and the level on one runtime material.
    EXPECT_NE( code.find( "SetMaterial( drawn->GetMetadata().Handle" ), std::string::npos )
         << "the preview must be pushed the working copy's handle";
    EXPECT_EQ( code.find( "SetMaterial( Subject()" ), std::string::npos )
         << "pushing the SUBJECT at the preview puts the window and the level back on one runtime "
            "material, and an edit reaches both again";

    // The parameter table edits the working copy.
    EXPECT_NE( code.find( "DrawParameters( *drawn" ), std::string::npos );

    // THE SCENE IS REACHED FROM EXACTLY ONE PLACE, and that place is Apply.
    const std::string publishSubject = "PublishToRuntime( *subject";
    ASSERT_EQ( CountOf( code, publishSubject ), 1u )
         << "publishing the subject at the runtime IS the scene changing; more than one site means the "
            "scene can change somewhere other than Apply";
    EXPECT_EQ( EnclosingFunction( code, code.find( publishSubject ) ), "ApplyEdits" )
         << "the one site that changes the scene must be Apply and nothing else";

    // And the old function is gone rather than left beside its replacement (DC 4).
    EXPECT_EQ( code.find( "PropagateEdit" ), std::string::npos )
         << "the one-wire propagation must be deleted by the change that replaces it, not kept alive";
}

// ── 3b. ONE WRITE, TWO WAYS TO CALL IT ──────────────────────────────────────────────────────────────────
//
// A value now reaches this material from two places: the slider a person drags, and a `set` arriving on
// the control channel. The rule that keeps the second one honest is that it is not a second EXECUTION
// path -- both land in WriteParam, which is where the publish (and, one day, the undo entry) lives.
//
// A copy of the write in the channel's own code would be correct the day it was written and wrong the day
// somebody adds a step to the widget's, and the symptom would be a scripted capture proving a route
// nobody uses. So the count is asserted, not the intention.

TEST( MaterialEditStates, AValueReachesTheMaterialThroughExactlyOneWrite )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "could not locate the repository root from the working directory";

    const std::string code =
         CodeOnly( root + "Editor/Source/Editor/Panels/MaterialEditor/MaterialEditorPanel.cpp" );
    ASSERT_FALSE( code.empty() ) << "MaterialEditorPanel.cpp could not be read";

    const std::size_t writes = CountOf( code, "SetParam(" );
    ASSERT_EQ( writes, 1u )
         << "a parameter value must be written in exactly one place. A second SetParam site is a second "
            "route into the working copy, and the two will differ on the day one of them grows a step.";

    EXPECT_EQ( EnclosingFunction( code, code.find( "SetParam(" ) ), "WriteParam" )
         << "and that one place is WriteParam, which both the widget and the control channel call";

    // The channel's entry point does not write; it checks and then CALLS the write.
    EXPECT_NE( code.find( "WriteParam( *p, written )" ), std::string::npos )
         << "SetEditableProperty must reach the value through WriteParam, not around it";
}

// ── 3c. WHAT THE SCENE IS TOLD, AND WHEN ────────────────────────────────────────────────────────────────
//
// The global material stamp makes every mesh in every open scene throw away its cached material instances.
// A drag on the WORKING COPY was moving it on every frame -- for values that by construction never leave
// this window. Measured on a 30-step drag through the control channel: 30 stamps before, 0 after.
//
// THIS IS THE HARDEST KIND OF DEFECT TO SEE, which is why it is a named rule rather than an `if`: the
// pictures are identical whether the stamp moves or not. Only a counter tells them apart, so only a test
// can hold it.

TEST( MaterialEditStates, OnlyAnEditTheSceneCanSeeOwesTheSceneItsStamp )
{
    // The working copy of a BASE material: nothing outside this window caches its values. Its runtime
    // Material is re-valued in place, and the preview reads through it.
    EXPECT_FALSE( MaterialEdit::PublishOwesTheGlobalStamp( /*isInstance=*/false, /*isSubject=*/false ) )
         << "a drag on the working copy must not make every mesh in the level rebuild its material "
            "instances -- the level is not showing that value and by design cannot be";

    // The SUBJECT: Apply. Child instances of it bake their overrides at creation and would otherwise keep
    // the values from before.
    EXPECT_TRUE( MaterialEdit::PublishOwesTheGlobalStamp( /*isInstance=*/false, /*isSubject=*/true ) );

    // An INSTANCE has no runtime Material to re-value; its overrides live in the cached MaterialInstance,
    // and dropping that cache is the only lever there is. That is true of its working copy too -- the
    // preview's own entity holds one -- so this case still pays, and the header says so rather than
    // pretending the cost is gone.
    EXPECT_TRUE( MaterialEdit::PublishOwesTheGlobalStamp( /*isInstance=*/true, /*isSubject=*/false ) );
    EXPECT_TRUE( MaterialEdit::PublishOwesTheGlobalStamp( /*isInstance=*/true, /*isSubject=*/true ) );
}

TEST( MaterialEditStates, TheStampIsBumpedInExactlyOnePlaceAndOnlyBehindThatRule )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "could not locate the repository root from the working directory";

    const std::string code =
         CodeOnly( root + "Editor/Source/Editor/Panels/MaterialEditor/MaterialEditorPanel.cpp" );
    ASSERT_FALSE( code.empty() ) << "MaterialEditorPanel.cpp could not be read";

    // ONE SITE. A second would be a second opinion about who has to be told, and the two would differ on
    // the day one of them is updated -- silently, because the pictures agree either way.
    ASSERT_EQ( CountOf( code, "BumpInvalidationVersion" ), 1u );
    EXPECT_EQ( EnclosingFunction( code, code.find( "BumpInvalidationVersion" ) ), "PublishToRuntime" );

    // And it is behind the rule rather than beside it.
    EXPECT_NE( code.find( "PublishOwesTheGlobalStamp" ), std::string::npos );
    const std::size_t guard = code.rfind( "if ( owesTheStamp )", code.find( "BumpInvalidationVersion" ) );
    EXPECT_NE( guard, std::string::npos )
         << "the bump must be conditional on the rule; an unconditional one restores the cost this change "
            "measured away";
}

// ── 4. THE PROPERTY CENSUS IS THE SHADER'S SCHEMA, and nothing beside it ────────────────────────────────
//
// The control channel can now write a document's properties (Editor/Core/EditableProperty.hpp for why
// that is a second CATEGORY of request rather than a second execution path). What it may write is DERIVED
// from the same `Properties` block MaterialEditorPanel::DrawParameters builds its rows from, and this is
// the side of that which a suite can hold: a census over a schema, with no shader service, no GPU and no
// ImGui in sight.
//
// It matters because the alternative -- a list of parameter names belonging to the channel -- is the
// defect shape this project spends its days removing. Two lists agree on the day they are written; the
// one nobody remembers falls behind, and what a client can set stops being what an artist can edit.

namespace
{
    namespace Formats = Desert::Core::Formats;

    Formats::ShaderParam Value( const char* name, Formats::ShaderValueType type )
    {
        Formats::ShaderParam p;
        p.Name = name;
        p.Type = type;
        return p;
    }

    Formats::ShaderParam Ranged( const char* name, float min, float max, float def )
    {
        Formats::ShaderParam p = Value( name, Formats::ShaderValueType::Float );
        p.Min                  = min;
        p.Max                  = max;
        p.Default              = glm::vec4( def, 0.0f, 0.0f, 0.0f );
        return p;
    }

    Formats::ShaderParam Texture( const char* name )
    {
        Formats::ShaderParam p = Value( name, Formats::ShaderValueType::Float );
        p.IsTexture            = true;
        return p;
    }

    Formats::ShaderParam AssetRef( const char* name, const char* kind )
    {
        Formats::ShaderParam p = Value( name, Formats::ShaderValueType::Float );
        p.AssetKind            = kind;
        return p;
    }

    Formats::ShaderProgramMeta SchemaOf( std::vector<Formats::ShaderParam> params )
    {
        Formats::ShaderProgramMeta schema;
        schema.Params = std::move( params );
        return schema;
    }

    const Desert::Editor::EditableProperty* Find( const std::vector<Desert::Editor::EditableProperty>& properties,
                                                  const char*                                          name )
    {
        for ( const auto& property : properties )
        {
            if ( property.Name == name )
                return &property;
        }
        return nullptr;
    }
} // namespace

TEST( MaterialEditStates, TheCensusIsExactlyTheSchemaAndCarriesWhatTheWindowShows )
{
    Formats::ShaderParam colour = Value( "AlbedoColor", Formats::ShaderValueType::Float4 );
    colour.Widget               = Formats::ShaderParamWidget::Color;
    colour.DisplayName          = "Base Colour";
    colour.Default              = glm::vec4( 1.0f );

    const auto schema = SchemaOf( { Ranged( "RoughnessFactor", 0.0f, 1.0f, 0.5f ), colour, Texture( "AlbedoMap" ),
                                    AssetRef( "CloudType", "CloudTypeAsset" ) } );

    MaterialData data;
    data.SetParam( "RoughnessFactor", glm::vec4( 0.25f, 0.0f, 0.0f, 0.0f ) );

    const auto census = MaterialEdit::DescribeProperties( schema, data, nullptr, /*isInstance=*/false );

    // EVERY declared property is present, in the order THE WINDOW DRAWS ITS ROWS -- so a client walking
    // this list and a person reading the panel walk the same rows.
    //
    // That order stopped being declaration order when the table gained groups: the asset reference is
    // hoisted into the leading "Inputs" group, and the three values follow in their own. This assertion
    // used to read RoughnessFactor, AlbedoColor, AlbedoMap, CloudType, and updating it is the point rather
    // than a cost -- had the census kept the old order while the window moved, this suite would have gone
    // on passing while the relation it exists to protect was broken.
    ASSERT_EQ( census.size(), 4u );
    EXPECT_EQ( census[0].Name, "CloudType" ) << "an asset reference is an input and comes first";
    EXPECT_EQ( census[0].Group, "Inputs" );
    EXPECT_EQ( census[1].Name, "RoughnessFactor" );
    EXPECT_EQ( census[2].Name, "AlbedoColor" );
    EXPECT_EQ( census[3].Name, "AlbedoMap" ) << "a texture is a value and keeps its declared place";

    // The value the WINDOW is showing: this material's own override where it has one...
    const auto* roughness = Find( census, "RoughnessFactor" );
    ASSERT_NE( roughness, nullptr );
    EXPECT_EQ( roughness->Components, 1 );
    EXPECT_FLOAT_EQ( roughness->Value[0], 0.25f );
    ASSERT_TRUE( roughness->Min.has_value() );
    EXPECT_FLOAT_EQ( *roughness->Min, 0.0f );
    EXPECT_TRUE( roughness->Settable );

    // ...and the SCHEMA DEFAULT where it has none. Not zero: a property nobody has touched is showing the
    // shader's own value, and a census that reported 0 would have a client "correcting" a material to a
    // number it was never displaying.
    const auto* albedo = Find( census, "AlbedoColor" );
    ASSERT_NE( albedo, nullptr );
    EXPECT_EQ( albedo->Type, "color" ) << "the widget refines the type on the wire; four floats are not metres";
    EXPECT_EQ( albedo->Label, "Base Colour" ) << "the label a person reads, when the schema declares one";
    EXPECT_EQ( albedo->Components, 4 );
    EXPECT_FLOAT_EQ( albedo->Value[3], 1.0f );
}

TEST( MaterialEditStates, WhatCannotBeWrittenIsLISTEDWithAReasonRatherThanOmitted )
{
    const auto schema = SchemaOf( { Texture( "AlbedoMap" ), AssetRef( "CloudType", "CloudTypeAsset" ) } );
    const auto census = MaterialEdit::DescribeProperties( schema, MaterialData{}, nullptr, false );

    // A property MISSING from the census reads as a property the shader does not declare, and those are
    // two different problems with two different fixes. So both appear, both say no, and both say why.
    ASSERT_EQ( census.size(), 2u );
    for ( const auto& property : census )
    {
        EXPECT_FALSE( property.Settable ) << property.Name;
        EXPECT_FALSE( property.NotSettableReason.empty() )
             << property.Name << ": a refusal with nothing said is the one thing this channel forbids";
        EXPECT_EQ( property.Components, 0 ) << "an asset handle is not a float, and must not be shown as one";
    }
    EXPECT_EQ( Find( census, "AlbedoMap" )->Type, "texture" );
    EXPECT_EQ( Find( census, "CloudType" )->Type, "CloudTypeAsset" ) << "the asset KIND is what a client needs";
}

TEST( MaterialEditStates, AnInstanceShowsTheParentsValueAndMarksItsOwnOverrides )
{
    const auto schema = SchemaOf(
         { Ranged( "RoughnessFactor", 0.0f, 1.0f, 0.5f ), Ranged( "MetallicFactor", 0.0f, 1.0f, 0.0f ) } );

    MaterialData parent;
    parent.SetParam( "RoughnessFactor", glm::vec4( 0.9f, 0.0f, 0.0f, 0.0f ) );
    parent.SetParam( "MetallicFactor", glm::vec4( 0.8f, 0.0f, 0.0f, 0.0f ) );

    MaterialData child;
    child.SetParam( "MetallicFactor", glm::vec4( 0.1f, 0.0f, 0.0f, 0.0f ) );

    const auto census = MaterialEdit::DescribeProperties( schema, child, &parent, /*isInstance=*/true );

    // The seeding rule is the window's own (MaterialEdit::EffectiveParamValue), which is why it lives in
    // one function both call: a client reading a number the panel is not displaying is worse than a client
    // reading no number at all, because a report quotes it beside a picture.
    const auto* inherited = Find( census, "RoughnessFactor" );
    ASSERT_NE( inherited, nullptr );
    EXPECT_FLOAT_EQ( inherited->Value[0], 0.9f );
    EXPECT_FALSE( inherited->OverridesParent );

    const auto* overridden = Find( census, "MetallicFactor" );
    ASSERT_NE( overridden, nullptr );
    EXPECT_FLOAT_EQ( overridden->Value[0], 0.1f );
    EXPECT_TRUE( overridden->OverridesParent )
         << "0.1 means something different depending on which of the two it is, and a client cannot "
            "otherwise tell an override from an inheritance";

    // An instance's VALUE params are precisely what it may override, so instance mode adds no refusal.
    EXPECT_TRUE( inherited->Settable );
    EXPECT_TRUE( overridden->Settable );
}

TEST( MaterialEditStates, ANameTheSchemaDoesNotDeclareIsRefusedAndTheRefusalNamesWhatThereIs )
{
    const auto schema = SchemaOf( { Ranged( "RoughnessFactor", 0.0f, 1.0f, 0.5f ), Texture( "AlbedoMap" ) } );

    std::string refusal;
    EXPECT_NE( MaterialEdit::FindSettableParam( schema, "RoughnessFactor", false, refusal ), nullptr );
    EXPECT_TRUE( refusal.empty() );

    // WRITTEN AND NEVER READ IS THE FAILURE THIS PREVENTS. A misspelt name stored in the material would be
    // serialised, ignored by every shader, and the capture taken to prove the edit would render as
    // "nothing moved" -- indistinguishable from the feature being broken.
    EXPECT_EQ( MaterialEdit::FindSettableParam( schema, "RoughnesFactor", false, refusal ), nullptr );
    EXPECT_NE( refusal.find( "RoughnesFactor" ), std::string::npos ) << refusal;
    EXPECT_NE( refusal.find( "RoughnessFactor" ), std::string::npos )
         << "the offer is built from the schema just walked, so it cannot go stale: " << refusal;
    EXPECT_EQ( refusal.find( "AlbedoMap" ), std::string::npos )
         << "offering a texture as an alternative to a value would send the caller into a second refusal";

    // A row the census marks unsettable refuses through the SAME function, so the list a client reads and
    // the answer it gets cannot disagree about one row.
    EXPECT_EQ( MaterialEdit::FindSettableParam( schema, "AlbedoMap", false, refusal ), nullptr );
    EXPECT_FALSE( refusal.empty() );
}

TEST( MaterialEditStates, TheComponentCountIsOneRuleAndItIsThePropertysIdentity )
{
    using VT = Formats::ShaderValueType;

    // Asked by the census, by the refusal that counts a caller's numbers, and by the widget. Three numbers
    // for a float is a caller who meant a different property, and finding that out from the picture is
    // finding it out too late.
    EXPECT_EQ( MaterialEdit::ComponentsOf( VT::Float ), 1 );
    EXPECT_EQ( MaterialEdit::ComponentsOf( VT::Float2 ), 2 );
    EXPECT_EQ( MaterialEdit::ComponentsOf( VT::Float3 ), 3 );
    EXPECT_EQ( MaterialEdit::ComponentsOf( VT::Float4 ), 4 );
    EXPECT_EQ( MaterialEdit::ComponentsOf( VT::Int ), 1 );
    EXPECT_EQ( MaterialEdit::ComponentsOf( VT::Bool ), 1 );

    // A schema the parser could not type must not be guessed WIDER than the narrowest thing it could be:
    // a four-component write into a one-component field would overwrite three neighbours' worth of nothing
    // and read as accepted.
    EXPECT_EQ( MaterialEdit::ComponentsOf( VT::Unknown ), 1 );
}

// ── 5. The reset, and the three things "default" names in this window (M7) ──────────────────────────────
//
// ONE SENTENCE, ASSERTED FROM BOTH SIDES: a reset is offered exactly when the row is showing something
// other than what it inherits. What it inherits is the SHADER's declared default on a base material and
// the PARENT chain's value on an instance — two different values, and Details' Д29 reset has only the
// first of them, which is why its rule could not be copied here and had to be generalised.
//
// The mistake this section exists to catch is the one a straight copy of Д29 makes: reset by WRITING THE
// DEFAULT IN rather than by removing the entry. On a base material the two are indistinguishable, which is
// exactly how such a bug ships; on an INSTANCE the copy hands the row the schema default while the window
// is displaying the parent's value, so the picture jumps to a third number nothing was showing.

TEST( MaterialEditStates, TheResetIsOfferedExactlyWhenTheRowIsNotShowingWhatItInherits )
{
    const auto  schema   = SchemaOf( { Ranged( "Coverage", 0.0f, 1.0f, 0.45f ) } );
    const auto& coverage = schema.Params[0];

    // A base material that says nothing: the row IS the shader's default, so there is nothing to hand back.
    MaterialData silent;
    EXPECT_EQ( MaterialEdit::ResetOfferedFor( silent, nullptr, coverage, /*isInstance=*/false ),
               MaterialEdit::RowReset::None );

    // A base material that stores the default EXPLICITLY still gets no arrow, and that is the measured half
    // of this rule rather than an oversight. This repository's own M_O4_Both_Clouds.demat carries 28 stored
    // parameters of which 23 are byte-equal to the shader's default — written by the migration that moved
    // the cloud look off the component, not by an artist. "The material stores an entry" would put 28
    // arrows on a material with 5 real deviations, and an arrow on every row is an arrow on none.
    MaterialData redundant;
    redundant.SetParam( "Coverage", glm::vec4( 0.45f, 0.0f, 0.0f, 0.0f ) );
    EXPECT_EQ( MaterialEdit::ResetOfferedFor( redundant, nullptr, coverage, /*isInstance=*/false ),
               MaterialEdit::RowReset::None );

    // And a value that differs does get one, named for what it would hand back.
    MaterialData authored;
    authored.SetParam( "Coverage", glm::vec4( 0.762f, 0.0f, 0.0f, 0.0f ) );
    EXPECT_EQ( MaterialEdit::ResetOfferedFor( authored, nullptr, coverage, /*isInstance=*/false ),
               MaterialEdit::RowReset::ToShaderDefault );
}

TEST( MaterialEditStates, AnInstanceResetsToItsPARENTAndNotToTheShadersDefault )
{
    const auto  schema   = SchemaOf( { Ranged( "Coverage", 0.0f, 1.0f, 0.45f ) } );
    const auto& coverage = schema.Params[0];

    // THREE DISTINCT NUMBERS ON PURPOSE — schema 0.45, parent 0.80, child 0.20. A reset that wrote the
    // schema default in would land on 0.45, which is neither what the row shows nor what it inherits, and
    // no test with parent == default could ever tell the two implementations apart.
    MaterialData parent;
    parent.SetParam( "Coverage", glm::vec4( 0.80f, 0.0f, 0.0f, 0.0f ) );

    MaterialData child;
    child.SetParam( "Coverage", glm::vec4( 0.20f, 0.0f, 0.0f, 0.0f ) );

    EXPECT_EQ( MaterialEdit::ResetOfferedFor( child, &parent, coverage, /*isInstance=*/true ),
               MaterialEdit::RowReset::ToParentValue );
    EXPECT_FLOAT_EQ( MaterialEdit::EffectiveParamValue( child, &parent, coverage ).x, 0.20f );

    // THE RESET IS THE ERASE, and this is the relation the whole feature rests on: after it, the row shows
    // what it inherits, and therefore offers no further reset. Both halves, because either alone passes for
    // a broken implementation — a reset that erases nothing satisfies the second, and one that writes 0.45
    // in satisfies neither but would satisfy a test written against the schema default.
    EXPECT_TRUE( child.RemoveParam( "Coverage" ) );
    EXPECT_FLOAT_EQ( MaterialEdit::EffectiveParamValue( child, &parent, coverage ).x, 0.80f )
         << "an instance falls back to its PARENT, not to the shader — 0.45 here means the reset wrote the "
            "schema default in instead of removing the entry";
    EXPECT_EQ( MaterialEdit::ResetOfferedFor( child, &parent, coverage, /*isInstance=*/true ),
               MaterialEdit::RowReset::None );

    // The same row, same bytes, read as a BASE material: it now inherits from the shader instead, so a
    // parent value of 0.80 is not involved at all. The two modes are two answers to one question and the
    // predicate must not average them.
    MaterialData asBase;
    asBase.SetParam( "Coverage", glm::vec4( 0.80f, 0.0f, 0.0f, 0.0f ) );
    EXPECT_EQ( MaterialEdit::ResetOfferedFor( asBase, nullptr, coverage, /*isInstance=*/false ),
               MaterialEdit::RowReset::ToShaderDefault );
}

TEST( MaterialEditStates, RemovingIsNotWritingTheDefaultIn )
{
    // The two are indistinguishable in the PICTURE and different in the FILE, which is why this is asserted
    // on the data rather than left to the render. A material that stores its inherited value has PINNED it:
    // editing the parent, or the shader, stops reaching it. That is the opposite of a reset.
    MaterialData data;
    data.SetParam( "Coverage", glm::vec4( 0.762f, 0.0f, 0.0f, 0.0f ) );
    data.SetParam( "Seed", glm::vec4( 7.0f, 0.0f, 0.0f, 0.0f ) );
    ASSERT_EQ( data.Params.size(), 2u );

    EXPECT_TRUE( data.RemoveParam( "Coverage" ) );
    EXPECT_EQ( data.Params.size(), 1u ) << "the entry is gone, not overwritten";
    EXPECT_EQ( data.FindParam( "Coverage" ), nullptr );
    EXPECT_EQ( data.Params[0].Name, "Seed" ) << "the surviving entries keep their order";

    // A reset that changed nothing records nothing — the same discipline ResetFieldToDefault has in
    // Details, and what lets MaterialEditorPanel::ResetParam skip the publish for a no-op.
    EXPECT_FALSE( data.RemoveParam( "Coverage" ) );
    EXPECT_FALSE( data.RemoveParam( "NeverWritten" ) );
}

TEST( MaterialEditStates, RowsThatCannotBeResetAreRefusedByKindAndNotByValue )
{
    // TEXTURE AND ASSET-REFERENCE ROWS GET NO ARROW, and the two reasons WERE different. A cloud type or
    // layout slot already carries its own empty entry in its combo, so an arrow would be a second control
    // for one action.
    //
    // THE SECOND REASON HAS EXPIRED, AND THIS ASSERTION IS NOW PINNING A DECISION RATHER THAN A LIMIT.
    // It used to read: a 2D texture slot cannot be UNBOUND at all, because
    // Graphic::DataDrivenMaterial::SetTexture refused a null image and MaterialFactory::ApplyShaderAsset
    // skipped handle 0 — so erasing the entry would clear the file and leave the ball still sampling the
    // old texture, a control that changes the document and not the picture (DC §1.3). М9 removed that
    // limit: a null image now binds the shader's own `Properties … = "white"` default
    // (Material::BindSchemaDefaultTexture), measured on a live editor as 100 % of the floor pixels moving
    // where dev's behaviour moved 0.4 %.
    //
    // So "reset this texture to the shader default" is a real, executable operation today, and whether the
    // arrow should be OFFERED for a texture row is a MaterialEditor decision — that panel is not М9's, and
    // an assertion changed without the behaviour it describes would be a test written to pass. Left as is,
    // deliberately, with the reason spelled out so the next reader is not told a defect exists that does
    // not.
    const auto schema = SchemaOf( { Texture( "AlbedoMap" ), AssetRef( "CloudType1", "CloudTypeAsset" ) } );

    MaterialData bound;
    bound.SetTexture( "AlbedoMap", Common::Content::AssetGuid{ 0, 1234u }, "" );
    bound.SetCloudAsset( "CloudType1", Common::Content::AssetGuid{ 0, 5678u }, "" );

    for ( const auto& p : schema.Params )
    {
        EXPECT_EQ( MaterialEdit::ResetOfferedFor( bound, nullptr, p, /*isInstance=*/false ),
                   MaterialEdit::RowReset::None )
             << p.Name << ": offered a reset it cannot carry out";
        EXPECT_EQ( MaterialEdit::ResetOfferedFor( bound, nullptr, p, /*isInstance=*/true ),
                   MaterialEdit::RowReset::None )
             << p.Name << ": an instance takes these whole from its parent and draws them read-only";
    }
}

TEST( MaterialEditStates, EveryOfferedResetHasSomethingToSayAboutItself )
{
    // The arrow is one glyph and it means one of two different things depending on the document. An artist
    // pressing it has to know WHICH before pressing, so a kind with no sentence is a kind that ships a
    // control nobody can predict — and adding a third kind without a sentence is what this catches.
    for ( const auto kind : { MaterialEdit::RowReset::ToShaderDefault, MaterialEdit::RowReset::ToParentValue } )
    {
        const std::string tooltip = MaterialEdit::ResetTooltip( kind );
        EXPECT_FALSE( tooltip.empty() );
        EXPECT_NE( tooltip.find( kind == MaterialEdit::RowReset::ToParentValue ? "parent" : "shader" ),
                   std::string::npos )
             << "the sentence must name WHICH default it is about: " << tooltip;
    }

    // A row that offers nothing says nothing: the arrow is not drawn, so there is no hover to explain.
    EXPECT_STREQ( MaterialEdit::ResetTooltip( MaterialEdit::RowReset::None ), "" );
}

TEST( MaterialEditStates, TheResetGoesThroughTheOneUnwriteAndLandsInTheWORKINGCopy )
{
    // SOURCE-LEVEL, for the reason the one-write assertion above gives: MaterialEditorPanel.cpp owns a
    // PreviewViewport, which owns a SceneRenderer, which needs a Vulkan device — so no suite can construct
    // the panel. What is checkable is that the drawing site does not grow a second execution path.
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "could not find the repository root from the test's working directory";

    const std::string code =
         CodeOnly( root + "Editor/Source/Editor/Panels/MaterialEditor/MaterialEditorPanel.cpp" );
    ASSERT_FALSE( code.empty() );

    // ONE call site for the removal, and it is inside ResetParam. A row that erased the entry itself would
    // skip the publish, and the value would revert in the file while the ball kept the old picture — the
    // silent half-edit Д29 was about, in this window's own vocabulary.
    EXPECT_EQ( CountOf( code, "RemoveParam(" ), 1u )
         << "the erase belongs to ResetParam alone; a second caller is a second execution path";

    const std::size_t at = code.find( "RemoveParam(" );
    ASSERT_NE( at, std::string::npos );
    EXPECT_EQ( EnclosingFunction( code, at ), "ResetParam" );

    // AND IT REACHES THE DRAWN MATERIAL, never the subject. The whole point of the staged document is that
    // an edit lands where only this window's preview can see it; a reset that went to the subject would put
    // every mesh in every open scene back to the shader's defaults with no Apply and no way back.
    const std::size_t body = code.find( "MaterialEditorPanel::ResetParam" );
    ASSERT_NE( body, std::string::npos );
    const std::string reset = code.substr( body, code.find( "\n    }", body ) - body );
    EXPECT_NE( reset.find( "DrawnMaterial()" ), std::string::npos )
         << "the reset must resolve its own target, so no caller can hand in the subject";
    EXPECT_EQ( reset.find( "ResolveSubject()" ), std::string::npos )
         << "a reset that reached the subject would move the level with no Apply and no Discard";
    EXPECT_NE( reset.find( "PublishToRuntime(" ), std::string::npos )
         << "without the publish the value reverts in the document and not on screen";
}

// ── 6. A drop the cloud slots cannot take is ANSWERED, not swallowed (M7) ───────────────────────────────
//
// The defect: FileExplorerPanel types its drag payload by FileType and every image is FileType::Texture, so
// the browser emits TEXTURE_ASSET for a `.png` while these slots accepted only the generic AssetFile — and
// a payload id that does not match fails SILENTLY in ImGui. Nothing bound, nothing logged, nothing drawn.
// The same defect had already been found and fixed in CloudLayoutPanel's own image slots; the material's
// slots kept it, which makes this the "one symptom fixed, its neighbour left standing" shape as well.
//
// Two halves are testable and both are held here: the SENTENCE (pure, below) and the fact that the panel
// hands it the real extension constants rather than something that merely looks like them.

namespace
{
    // What the panel passes, spelled once here so the expectations below and the source check agree.
    constexpr const char* kTypeExt   = ".decloudtype";
    constexpr const char* kLayoutExt = ".dclayout";

    std::string RefusalFor( const char* path, bool isType )
    {
        return MaterialEdit::WhyThatCannotGoInThisSlot( path, isType, kTypeExt, kLayoutExt );
    }
} // namespace

TEST( MaterialEditStates, APictureDroppedOnALayoutSlotIsAnsweredWithTheRouteThatWorks )
{
    // THE CASE THE FUNCTION EXISTS FOR. A refusal that only says "no" leaves the artist where they were —
    // and where they were is "I cannot add a texture to a cloud material". So the sentence has to carry the
    // step that does work, because for a picture there IS one.
    const std::string refusal = RefusalFor( "Assets/Textures/sky_bands.png", /*isType=*/false );

    EXPECT_NE( refusal.find( "sky_bands.png" ), std::string::npos )
         << "name what arrived, or the artist cannot tell which of two drags was refused: " << refusal;
    EXPECT_NE( refusal.find( kLayoutExt ), std::string::npos ) << "name what the slot takes: " << refusal;
    EXPECT_NE( refusal.find( "Pattern image" ), std::string::npos )
         << "name the step that gets the picture in — the whole point of answering at all: " << refusal;
    EXPECT_NE( refusal.find( "New Cloud Asset" ), std::string::npos )
         << "and where a layout comes from when there is not one yet: " << refusal;

    // WHY it is refused rather than imported here, in the sentence: a layout is not an image. It carries
    // four species channels AND an add/remove mask, which is exactly why CloudLayoutPanel imports the
    // pattern and the mask as two separate pictures (O-4). An artist told only "no" would reasonably think
    // the editor was broken.
    EXPECT_NE( refusal.find( "mask" ), std::string::npos ) << refusal;
}

TEST( MaterialEditStates, EveryRefusedDropSaysWhatArrivedAndWhatTheSlotTakes )
{
    struct Case
    {
        const char* Path;
        bool        IsType;
        const char* Wanted;
    };

    // The four families that reach these slots: a picture (both slots), a sibling cloud format on the same
    // generic payload, and anything else. None of them may produce an empty string — a refusal with nothing
    // said is the one thing DC §1.4 forbids outright, and it is what this row did before.
    const Case cases[] = {
         { "Assets/Textures/sky.png", false, kLayoutExt },
         { "Assets/Textures/sky.png", true, kTypeExt },
         { "Assets/Clouds/Cirrus.decloudtype", false, kLayoutExt },
         { "Assets/Clouds/Layouts/Bands.dclayout", true, kTypeExt },
         { "Assets/Clouds/CloudNoise_Default.dcnv", false, kLayoutExt },
         { "Assets/Scenes/Clouds_Demo.desce", true, kTypeExt },
    };

    for ( const auto& c : cases )
    {
        const std::string refusal = RefusalFor( c.Path, c.IsType );
        EXPECT_FALSE( refusal.empty() ) << c.Path;
        EXPECT_NE( refusal.find( std::filesystem::path( c.Path ).filename().string() ), std::string::npos )
             << c.Path << " -> " << refusal;
        EXPECT_NE( refusal.find( c.Wanted ), std::string::npos )
             << "the slot must name what it DOES take, or the refusal is a dead end: " << refusal;
    }

    // And the near miss is named as the near miss it is: a `.decloudtype` on a layout slot is not "not
    // something this slot can take", it is the OTHER slot's file, and saying so is the fix.
    const std::string swapped = RefusalFor( "Assets/Clouds/Cirrus.decloudtype", /*isType=*/false );
    EXPECT_NE( swapped.find( kTypeExt ), std::string::npos ) << swapped;
    EXPECT_NE( swapped.find( "painted map of the sky" ), std::string::npos ) << swapped;
}

TEST( MaterialEditStates, TheSlotAcceptsThePayloadTheBrowserActuallyEmitsAndPassesTheRealExtensions )
{
    // SOURCE-LEVEL, for the reason every other panel claim here is: MaterialEditorPanel.cpp cannot be
    // linked by a suite. What it holds is the two halves of the defect that no pure function can see.
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    const std::string code =
         CodeOnly( root + "Editor/Source/Editor/Panels/MaterialEditor/MaterialEditorPanel.cpp" );
    ASSERT_FALSE( code.empty() );

    const std::size_t at = code.find( "DrawCloudAssetRef" );
    ASSERT_NE( at, std::string::npos );
    const std::string row = code.substr( code.find( "MaterialEditorPanel::DrawCloudAssetRef" ) );

    // HALF ONE: the target must accept the payload the Content Browser really emits for an image. Accepting
    // only AssetFile is what made the drop invisible, and it is invisible in exactly the way that leaves no
    // evidence — so nothing but this line can catch a revert.
    EXPECT_NE( row.find( "DragPayloads::TextureAsset" ), std::string::npos )
         << "a .png travels as TEXTURE_ASSET; a slot that does not accept it cannot even refuse it";
    EXPECT_NE( row.find( "DragPayloads::AssetFile" ), std::string::npos )
         << "and .dclayout / .decloudtype travel as the generic AssetFile";

    // HALF TWO: the sentence is parameterised on the extensions so this header stays free of the cloud
    // includes — which means the panel could hand it anything. It must hand it the real constants.
    EXPECT_NE( row.find( "Assets::kCloudTypeExtension" ), std::string::npos );
    EXPECT_NE( row.find( "Assets::kCloudLayoutExtension" ), std::string::npos );
}

// ── 5. THE PARAMETER TABLE'S GROUPS ────────────────────────────────────────────────────────────────────
//
// `ShaderParam::Category` was filled in by shader authors and read by nothing: fifty-two params across the
// shipped shaders carry one of nine values, and the Material Editor drew all of them in one flat table, so
// the cloud material's thirty-four arrived as a single undivided list. Desert/Tests/Engine/
// CloudMaterialSchema meanwhile asserted every cloud param HAS a category "so it does not land in an
// unnamed group" -- a claim about a value with no consumer, which is a claim about nothing.
//
// The claims worth holding here are all about RELATIONS rather than about the plan's return value:
//
//   - the plan is a PERMUTATION of the schema: nothing added, nothing lost, nothing drawn twice. A grouping
//     that silently dropped a parameter would remove a knob from the window and from the channel at once,
//     and both would look like the shader simply not declaring it;
//   - the plan's order IS the census's order, because the window and `properties` consume the same plan.
//     Two orders that agree by inspection is precisely what this codebase keeps paying for;
//   - "no categories in this shader" and "one group called nothing" stay distinguishable, because six
//     shipped shaders are in the first state and must keep the flat table they have always drawn.

TEST( MaterialEditStates, GroupsAreTheShadersOwnCategoriesInTheOrderTheFileDeclaresThem )
{
    Formats::ShaderParam coverage = Ranged( "Coverage", 0.0f, 1.0f, 0.45f );
    coverage.Category             = "Weather";
    Formats::ShaderParam seed     = Value( "Seed", Formats::ShaderValueType::Float );
    seed.Category                 = "Weather";
    Formats::ShaderParam albedo   = Ranged( "ScatteringAlbedo", 0.0f, 1.0f, 0.98f );
    albedo.Category               = "Lighting";
    Formats::ShaderParam gloss    = Ranged( "Glossiness", 0.0f, 1.0f, 0.5f );
    gloss.Category                = "Detail";

    // Declared Weather, Weather, Lighting, Detail -- so the GROUPS come out in that order of first
    // appearance, and their numbers with them. No table of preferred category names exists anywhere for
    // this to disagree with, which is the whole design: a second list is what falls behind.
    const auto groups = MaterialEdit::PlanParameterGroups( SchemaOf( { coverage, seed, albedo, gloss } ) );

    ASSERT_EQ( groups.size(), 3u );
    EXPECT_EQ( groups[0].Category, "Weather" );
    EXPECT_EQ( groups[1].Category, "Lighting" );
    EXPECT_EQ( groups[2].Category, "Detail" );

    ASSERT_TRUE( groups[0].Ordinal.has_value() );
    EXPECT_EQ( *groups[0].Ordinal, 0u );
    EXPECT_EQ( *groups[1].Ordinal, 1u );
    EXPECT_EQ( *groups[2].Ordinal, 2u );

    EXPECT_EQ( groups[0].Params, ( std::vector<std::size_t>{ 0u, 1u } ) );
    EXPECT_TRUE( MaterialEdit::HasNamedGroups( groups ) );
}

TEST( MaterialEditStates, AssetReferencesAreHoistedIntoOneInputsGroupInFrontOfTheAuthorsStages )
{
    Formats::ShaderParam type1    = AssetRef( "CloudType1", "CloudTypeAsset" );
    type1.Category                = "Cloud Types";
    Formats::ShaderParam type2    = AssetRef( "CloudType2", "CloudTypeAsset" );
    type2.Category                = "Cloud Types";
    Formats::ShaderParam coverage = Ranged( "Coverage", 0.0f, 1.0f, 0.45f );
    coverage.Category             = "Weather";
    Formats::ShaderParam pattern  = AssetRef( "LayoutPattern", "CloudLayoutAsset" );
    pattern.Category              = "Layout";
    Formats::ShaderParam repeats  = Ranged( "LayoutRepeats", 1.0f, 16.0f, 1.0f );
    repeats.Category              = "Layout";

    const auto groups =
         MaterialEdit::PlanParameterGroups( SchemaOf( { type1, type2, coverage, pattern, repeats } ) );

    // The dependencies come FIRST and TOGETHER -- "which clouds is this sky made of" answered in one place,
    // which is the shape the owner chose. Membership is IsAssetRef and nothing else, so a CloudType5 added
    // to the shader joins them without anybody editing a list here.
    ASSERT_EQ( groups.size(), 3u );
    EXPECT_EQ( groups[0].Kind, MaterialEdit::ParameterGroupKind::Inputs );
    EXPECT_EQ( groups[0].Category, "Inputs" );
    EXPECT_FALSE( groups[0].Ordinal.has_value() ) << "the numbers belong to the author's stages, not to this";
    EXPECT_EQ( groups[0].Params, ( std::vector<std::size_t>{ 0u, 1u, 3u } ) )
         << "the two types and the layout, in declaration order";

    // "Cloud Types" was nothing BUT its two slots, so the hoist emptied it and it is not drawn at all -- a
    // heading with nothing under it reads as a section that failed to load. "Layout" keeps its scalar.
    EXPECT_EQ( groups[1].Category, "Weather" );
    EXPECT_EQ( groups[2].Category, "Layout" );
    EXPECT_EQ( groups[2].Params, ( std::vector<std::size_t>{ 4u } ) );

    // And the numbering restarts at the first surviving authored group, with no hole where Cloud Types was.
    EXPECT_EQ( *groups[1].Ordinal, 0u );
    EXPECT_EQ( *groups[2].Ordinal, 1u );
}

TEST( MaterialEditStates, ATextureIsNotAnInputAndStaysInTheGroupItsAuthorChose )
{
    // The line between the two: an asset reference links to a document with its own window, a texture is a
    // value bound to this material. StaticMeshPBR's author already grouped its textures under "Textures";
    // hoisting them would be this code overruling a grouping that was made correctly.
    Formats::ShaderParam albedoMap = Texture( "AlbedoMap" );
    albedoMap.Category             = "Textures";
    Formats::ShaderParam normalMap = Texture( "NormalMap" );
    normalMap.Category             = "Textures";
    Formats::ShaderParam tint      = Ranged( "Tint", 0.0f, 1.0f, 1.0f );
    tint.Category                  = "Surface";

    const auto groups = MaterialEdit::PlanParameterGroups( SchemaOf( { tint, albedoMap, normalMap } ) );

    ASSERT_EQ( groups.size(), 2u ) << "no Inputs group at all: this shader references no documents";
    EXPECT_EQ( groups[0].Category, "Surface" );
    EXPECT_EQ( groups[1].Category, "Textures" );
    EXPECT_EQ( groups[1].Params, ( std::vector<std::size_t>{ 1u, 2u } ) );
    for ( const auto& group : groups )
        EXPECT_NE( group.Kind, MaterialEdit::ParameterGroupKind::Inputs );
}

TEST( MaterialEditStates, MembersOfOneCategoryMeetInOneGroupEvenWhenTheFileScattersThem )
{
    // No shipped shader interleaves its categories today. That is exactly the condition under which a plan
    // built by watching for the category to CHANGE looks correct forever and then opens "Weather" twice --
    // numbering the second copy as a new stage -- the first time somebody moves a line.
    Formats::ShaderParam first  = Value( "Coverage", Formats::ShaderValueType::Float );
    first.Category              = "Weather";
    Formats::ShaderParam middle = Value( "ScatteringAlbedo", Formats::ShaderValueType::Float );
    middle.Category             = "Lighting";
    Formats::ShaderParam last   = Value( "Seed", Formats::ShaderValueType::Float );
    last.Category               = "Weather";

    const auto groups = MaterialEdit::PlanParameterGroups( SchemaOf( { first, middle, last } ) );

    ASSERT_EQ( groups.size(), 2u ) << "one category is one group, however far apart its members were written";
    EXPECT_EQ( groups[0].Category, "Weather" );
    EXPECT_EQ( groups[0].Params, ( std::vector<std::size_t>{ 0u, 2u } ) )
         << "and the members keep their declaration order inside it";
    EXPECT_EQ( groups[1].Category, "Lighting" );
    EXPECT_EQ( *groups[1].Ordinal, 1u ) << "the number follows FIRST appearance, not the last one";
}

TEST( MaterialEditStates, AShaderThatCategorisedNothingHasOneUnnamedGroupAndIsDrawnFlat )
{
    // Terrain, Skybox, Unlit, TextSDF and the two MatProbes are all in this state. The window must keep
    // drawing them exactly as it did before groups existed -- an undivided table IS what "this shader has
    // no categories" looks like, and putting them under a heading would be this code inventing a fact.
    const auto groups =
         MaterialEdit::PlanParameterGroups( SchemaOf( { Value( "DetailTiling", Formats::ShaderValueType::Float ),
                                                        Texture( "u_GrassTex" ), Texture( "u_RockTex" ) } ) );

    ASSERT_EQ( groups.size(), 1u );
    EXPECT_TRUE( groups[0].Category.empty() );
    EXPECT_FALSE( groups[0].Ordinal.has_value() ) << "an unnamed group is not a stage of anybody's order";
    EXPECT_FALSE( MaterialEdit::HasNamedGroups( groups ) )
         << "which is the flag the window reads to draw the old flat table";
}

TEST( MaterialEditStates, UncategorisedParamsGetTheirOwnGroupAtTheEndRatherThanJoiningTheOneAboveThem )
{
    Formats::ShaderParam weather  = Value( "Coverage", Formats::ShaderValueType::Float );
    weather.Category              = "Weather";
    Formats::ShaderParam lighting = Value( "ScatteringAlbedo", Formats::ShaderValueType::Float );
    lighting.Category             = "Lighting";

    // The stray is declared FIRST, in the middle of nothing, and must still come out last: it is not a
    // stage of the author's work order and must not take a number inside it.
    const auto groups = MaterialEdit::PlanParameterGroups(
         SchemaOf( { Value( "Stray", Formats::ShaderValueType::Float ), weather, lighting } ) );

    ASSERT_EQ( groups.size(), 3u );
    EXPECT_EQ( groups[0].Category, "Weather" );
    EXPECT_EQ( groups[1].Category, "Lighting" );
    EXPECT_TRUE( groups[2].Category.empty() ) << "the leftovers go last, wherever they were written";
    EXPECT_FALSE( groups[2].Ordinal.has_value() );

    // And the numbers belong to the named groups only, with no hole where the stray sat.
    EXPECT_EQ( *groups[0].Ordinal, 0u );
    EXPECT_EQ( *groups[1].Ordinal, 1u );
}

TEST( MaterialEditStates, ThePlanIsAPermutationOfTheSchemaAndLosesNothing )
{
    Formats::ShaderParam a = Value( "A", Formats::ShaderValueType::Float );
    a.Category             = "Layout";
    Formats::ShaderParam b = Value( "B", Formats::ShaderValueType::Float );
    Formats::ShaderParam c = Value( "C", Formats::ShaderValueType::Float );
    c.Category             = "Layout";
    Formats::ShaderParam d = Value( "D", Formats::ShaderValueType::Float );
    d.Category             = "Detail";

    const auto schema = SchemaOf( { a, b, c, d } );
    const auto groups = MaterialEdit::PlanParameterGroups( schema );

    // THE BOUND WORTH ASSERTING, because it catches a whole class rather than an instance: every parameter
    // is drawn exactly once. A grouping that dropped one would take a knob out of the window AND out of the
    // channel's census at the same time, and both would read as the shader never declaring it.
    std::vector<std::size_t> flattened;
    for ( const auto& group : groups )
        flattened.insert( flattened.end(), group.Params.begin(), group.Params.end() );

    EXPECT_EQ( flattened.size(), schema.Params.size() ) << "nothing added, nothing lost";

    std::vector<std::size_t> sorted = flattened;
    std::sort( sorted.begin(), sorted.end() );
    EXPECT_TRUE( std::unique( sorted.begin(), sorted.end() ) == sorted.end() ) << "and nothing drawn twice";
    for ( std::size_t i = 0; i < sorted.size(); ++i )
        EXPECT_EQ( sorted[i], i ) << "every index of the schema appears";
}

TEST( MaterialEditStates, TheCensusWalksTheGroupsInTheSameOrderTheWindowDrawsThem )
{
    Formats::ShaderParam weather     = Value( "Coverage", Formats::ShaderValueType::Float );
    weather.Category                 = "Weather";
    Formats::ShaderParam lighting    = Value( "ScatteringAlbedo", Formats::ShaderValueType::Float );
    lighting.Category                = "Lighting";
    Formats::ShaderParam alsoWeather = Value( "Seed", Formats::ShaderValueType::Float );
    alsoWeather.Category             = "Weather";

    const auto schema =
         SchemaOf( { weather, lighting, alsoWeather, Value( "Stray", Formats::ShaderValueType::Float ) } );
    const auto census = MaterialEdit::DescribeProperties( schema, MaterialData{}, nullptr, /*isInstance=*/false );
    const auto groups = MaterialEdit::PlanParameterGroups( schema );

    // THE RELATION, ASSERTED RATHER THAN COMMENTED. A client reads `properties`, counts to the third entry
    // and speaks to a person about "the third row". Those are the same parameter only while the census and
    // the panel walk one order -- and since the panel now draws groups, declaration order is no longer it.
    std::vector<std::string> drawn;
    for ( const auto& group : groups )
    {
        for ( const std::size_t index : group.Params )
            drawn.push_back( schema.Params[index].Name );
    }

    ASSERT_EQ( census.size(), drawn.size() );
    for ( std::size_t i = 0; i < drawn.size(); ++i )
        EXPECT_EQ( census[i].Name, drawn[i] ) << "row " << i;

    EXPECT_EQ( drawn, ( std::vector<std::string>{ "Coverage", "Seed", "ScatteringAlbedo", "Stray" } ) )
         << "the scattered Weather member is pulled up to its group, and the stray falls to the end";

    // And every census row says which group it is under, so the position is readable and not inferred.
    EXPECT_EQ( census[0].Group, "Weather" );
    EXPECT_EQ( census[1].Group, "Weather" );
    EXPECT_EQ( census[2].Group, "Lighting" );
    EXPECT_TRUE( census[3].Group.empty() )
         << "empty is the shader declaring no category, which the window shows under a heading that says so";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
