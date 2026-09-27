// THE INSTANCING HLOD THE COOK BUILDS FOR EVERY CELL (WP10: Engine/Core/Serialize/WorldPartitionHLODRules.hpp,
// written by CookWorld in WorldCells.cpp).
//
// WHAT IS ASSERTED:
//
//   1. THE BATCHES. One InstancedStaticMesh record per distinct (mesh, materials, primitive, CastShadows); one
//      instance per StaticMesh at the record's WORLD matrix (a child of a rotated parent included), an authored
//      ISM's instances appended verbatim; batches in first-source order.
//   2. THE HOLES ARE NAMED. A record that draws and cannot be batched is listed with its reason from the closed
//      list; a record that draws nothing (no mesh, authored invisible) is neither batched nor listed.
//   3. THE INDEX ROW. The cell it stands in for, its file (absent when nothing batched), the sources in member
//      order, the instance count.
//   4. THE HLOD IS NOT THE WORLD. The records the index counts and the world assembled back are the source's
//      alone; the HLOD file is checked like a cell file (a damaged one is refused by name).
//   5. THE INDEX IS CHECKED. An HLOD row that names an always-loaded unit is refused by name.

#include <Engine/Core/Serialize/GenericBlock.hpp>
#include <Engine/Core/Serialize/WorldCells.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Json/Json.hpp>

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>

#include <rflcpp/rfl/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <numbers>
#include <span>
#include <string>
#include <vector>

using Desert::Assets::EntityData;
using Desert::Core::SceneSerialized;
using Desert::Core::WorldPartitionGridSerialized;
using Desert::Core::WorldPartitionSerialized;
namespace Cells = Desert::Core::WorldCells;
namespace Rules = Desert::Core::Rules;
namespace CC    = Common::Content;

namespace
{
    constexpr float       kCell     = 1000.0f;
    constexpr const char* kMaterial = "00000000000000000000000000004d41";
    constexpr const char* kSkinGuid = "000000000000000000000000000051b1";

    enum : std::uint64_t
    {
        kCamera = 1,
        kParent,    // cell (1,1), rotated, draws nothing
        kChild,     // its child: a cube
        kCubeA,     // cube
        kCubeB,     // cube
        kSphere,    // sphere
        kNoShadow,  // cube, CastShadows false
        kField,     // an authored ISM of cubes, two instances
        kHidden,    // cube with a hidden submesh
        kSkinned,   // skinned mesh
        kEdited,    // editor-built mesh
        kInvisible, // cube, Visibility false
        kLamp,      // no mesh at all
        kFar,       // cell (5,5): one cube
        kPrefab,    // unplaced prefab instance: the origin cell
        kEmptyCell, // cell (3,3): no mesh
    };

    glm::vec3 CellCentre( int column, int row )
    {
        return { ( static_cast<float>( column ) + 0.5f ) * kCell, 0.0f,
                 ( static_cast<float>( row ) + 0.5f ) * kCell };
    }

    EntityData Record( std::uint64_t id, const char* tag, glm::vec3 translation )
    {
        EntityData data;
        data.id          = Common::UUID( id );
        data.Tag         = tag;
        data.Translation = translation;
        return data;
    }

    Desert::Assets::StaticMeshComponentSer Cube()
    {
        Desert::Assets::StaticMeshComponentSer mesh;
        mesh.Primitive     = Desert::Geometry::PrimitiveType::Cube;
        mesh.MaterialGuids = std::vector<std::string>{ kMaterial };
        return mesh;
    }

    void WithMesh( EntityData& data, const Desert::Assets::StaticMeshComponentSer& mesh )
    {
        data.Components["StaticMesh"] = Common::Json::FromStruct( mesh );
    }

    std::array<float, 16> Flat( const glm::mat4& matrix )
    {
        std::array<float, 16> flat{};
        std::memcpy( flat.data(), &matrix[0][0], sizeof( float ) * 16 );
        return flat;
    }

    const glm::vec3 kFieldA = CellCentre( 1, 1 ) + glm::vec3( 200.0f, 0.0f, 200.0f );
    const glm::vec3 kFieldB = CellCentre( 1, 1 ) + glm::vec3( -200.0f, 0.0f, 200.0f );

    SceneSerialized World()
    {
        SceneSerialized scene;
        scene.SceneName      = "HlodMe";
        scene.WorldPartition = WorldPartitionSerialized{ { WorldPartitionGridSerialized{ kCell, 1500.0f } } };
        auto& records        = scene.Entities;

        EntityData camera           = Record( kCamera, "Camera", { 50.0f, 0.0f, 50.0f } );
        camera.Components["Camera"] = rfl::json::read<rfl::Generic>( R"({"IsMainCamera": true})" ).value();
        records.push_back( camera );

        EntityData parent = Record( kParent, "Parent", CellCentre( 1, 1 ) );
        parent.Rotation   = glm::vec3( 0.0f, std::numbers::pi_v<float> / 2.0f, 0.0f );
        records.push_back( parent );
        EntityData child = Record( kChild, "Child", { 100.0f, 0.0f, 0.0f } );
        child.parent     = Common::UUID( kParent );
        WithMesh( child, Cube() );
        records.push_back( child );

        EntityData cubeA = Record( kCubeA, "CubeA", CellCentre( 1, 1 ) + glm::vec3( 50.0f, 0.0f, 0.0f ) );
        WithMesh( cubeA, Cube() );
        records.push_back( cubeA );
        EntityData cubeB = Record( kCubeB, "CubeB", CellCentre( 1, 1 ) + glm::vec3( -50.0f, 0.0f, 0.0f ) );
        WithMesh( cubeB, Cube() );
        records.push_back( cubeB );

        EntityData sphere = Record( kSphere, "Sphere", CellCentre( 1, 1 ) + glm::vec3( 0.0f, 0.0f, 50.0f ) );
        auto       round  = Cube();
        round.Primitive   = Desert::Geometry::PrimitiveType::Sphere;
        WithMesh( sphere, round );
        records.push_back( sphere );

        EntityData noShadow =
             Record( kNoShadow, "NoShadow", CellCentre( 1, 1 ) + glm::vec3( 0.0f, 0.0f, -50.0f ) );
        auto dark        = Cube();
        dark.CastShadows = false;
        WithMesh( noShadow, dark );
        records.push_back( noShadow );

        EntityData                                      field = Record( kField, "Field", CellCentre( 1, 1 ) );
        Desert::Assets::InstancedStaticMeshComponentSer ism;
        ism.Primitive     = Desert::Geometry::PrimitiveType::Cube;
        ism.MaterialGuids = std::vector<std::string>{ kMaterial };
        ism.InstanceTransforms =
             std::vector<std::array<float, 16>>{ Flat( glm::translate( glm::mat4( 1.0f ), kFieldA ) ),
                                                 Flat( glm::translate( glm::mat4( 1.0f ), kFieldB ) ) };
        field.Components["InstancedStaticMesh"] = Common::Json::FromStruct( ism );
        records.push_back( field );

        EntityData hidden     = Record( kHidden, "Hidden", CellCentre( 1, 1 ) + glm::vec3( 10.0f, 0.0f, 10.0f ) );
        auto       holed      = Cube();
        holed.HiddenSubmeshes = 1;
        WithMesh( hidden, holed );
        records.push_back( hidden );

        EntityData skinned = Record( kSkinned, "Skinned", CellCentre( 1, 1 ) + glm::vec3( 20.0f, 0.0f, 20.0f ) );
        skinned.Components["SkinnedMesh"] =
             rfl::json::read<rfl::Generic>( std::string( R"({"MeshGuid": ")" ) + kSkinGuid + "\"}" ).value();
        records.push_back( skinned );

        EntityData edited = Record( kEdited, "Edited", CellCentre( 1, 1 ) + glm::vec3( 30.0f, 0.0f, 30.0f ) );
        Desert::Assets::StaticMeshComponentSer built;
        built.EditMesh = Desert::Geometry::EditMeshSer{};
        WithMesh( edited, built );
        records.push_back( edited );

        EntityData invisible =
             Record( kInvisible, "Invisible", CellCentre( 1, 1 ) + glm::vec3( 40.0f, 0, 40.0f ) );
        WithMesh( invisible, Cube() );
        invisible.Components["Visibility"] = rfl::json::read<rfl::Generic>( R"({"Visible": false})" ).value();
        records.push_back( invisible );

        records.push_back( Record( kLamp, "Lamp", CellCentre( 1, 1 ) + glm::vec3( -40.0f, 0.0f, -40.0f ) ) );

        EntityData distant = Record( kFar, "Far", CellCentre( 5, 5 ) );
        WithMesh( distant, Cube() );
        records.push_back( distant );

        EntityData prefab;
        prefab.id         = Common::UUID( kPrefab );
        prefab.Tag        = "Prefab";
        prefab.PrefabPath = "Prefabs/Thing.deprefab";
        records.push_back( prefab );

        records.push_back( Record( kEmptyCell, "Empty", CellCentre( 3, 3 ) ) );
        return scene;
    }

    struct Cooked
    {
        std::map<std::string, std::vector<unsigned char>> Files;
        Cells::WorldIndex                                 Index;
    };

    Cooked CookIt( const SceneSerialized& scene )
    {
        Cooked out;
        auto   cooked = Cells::CookWorld( scene, {} );
        EXPECT_TRUE( cooked.IsSuccess() ) << ( cooked.IsSuccess() ? "" : cooked.GetError() );
        if ( !cooked )
            return out;
        for ( const auto& file : cooked.GetValue().Files )
            out.Files[file.Name] = file.Bytes;
        auto index =
             Cells::ReadWorldIndex( Cells::kIndexFileName, out.Files.at( std::string( Cells::kIndexFileName ) ) );
        EXPECT_TRUE( index.IsSuccess() ) << ( index.IsSuccess() ? "" : index.GetError() );
        if ( index )
            out.Index = index.ExtractValue();
        return out;
    }

    Cells::FileReader ReaderOf( const std::map<std::string, std::vector<unsigned char>>& files )
    {
        return [&files]( std::string_view name ) -> Common::ResultStr<std::vector<unsigned char>>
        {
            const auto found = files.find( std::string( name ) );
            if ( found == files.end() )
                return Common::MakeError<std::vector<unsigned char>>( "no file '" + std::string( name ) + "'" );
            return Common::MakeSuccess( found->second );
        };
    }

    // The HLOD row standing in for the cell unit that holds record @p id.
    const Cells::IndexHLOD* HLODHolding( const Cells::WorldIndex& index, std::uint64_t id )
    {
        for ( const auto& hlod : index.HLODs )
            for ( const std::uint64_t held : index.Units.at( hlod.Unit ).Ids )
                if ( held == id )
                    return &hlod;
        return nullptr;
    }

    std::vector<Desert::Assets::InstancedStaticMeshComponentSer> BatchesOf( const Cooked&           cooked,
                                                                            const Cells::IndexHLOD& hlod )
    {
        std::vector<Desert::Assets::InstancedStaticMeshComponentSer> out;
        const std::size_t at      = static_cast<std::size_t>( &hlod - cooked.Index.HLODs.data() );
        auto              records = Cells::HLODRecords( cooked.Index, ReaderOf( cooked.Files ), at );
        EXPECT_TRUE( records.IsSuccess() ) << ( records.IsSuccess() ? "" : records.GetError() );
        if ( !records )
            return out;
        for ( const EntityData& record : records.GetValue() )
        {
            Common::Json::Issues issues;
            const rfl::Generic*  payload = nullptr;
            for ( const auto& [key, value] : record.Components )
                if ( key == "InstancedStaticMesh" )
                    payload = &value;
            EXPECT_NE( payload, nullptr );
            if ( payload == nullptr )
                continue;
            auto block = Desert::Core::Serialize::ReadBlock<Desert::Assets::InstancedStaticMeshComponentSer>(
                 Common::Json::Root( *payload ), issues );
            EXPECT_TRUE( block.has_value() && issues.empty() );
            if ( block )
                out.push_back( *block );
        }
        return out;
    }

    glm::vec3 TranslationOf( const std::array<float, 16>& flat )
    {
        return { flat[12], flat[13], flat[14] };
    }
} // namespace

TEST( WorldCellsHLOD, ACellsMeshesAreBatchedByWhatTheyDrawAtTheirWorldMatrices )
{
    const Cooked cooked = CookIt( World() );
    const auto*  hlod   = HLODHolding( cooked.Index, kCubeA );
    ASSERT_NE( hlod, nullptr );
    ASSERT_TRUE( hlod->File.has_value() );
    EXPECT_EQ( *hlod->File, Cells::HLODFileName( cooked.Index.Units.at( hlod->Unit ).File ) );

    const auto batches = BatchesOf( cooked, *hlod );
    ASSERT_EQ( batches.size(), 3u ); // cubes (+ the field), the sphere, the shadowless cube
    EXPECT_EQ( hlod->Ids.size(), 3u );
    EXPECT_EQ( hlod->Instances, 7u );

    const auto& cubes = batches[0];
    EXPECT_EQ( cubes.Primitive, Desert::Geometry::PrimitiveType::Cube );
    EXPECT_EQ( cubes.MaterialGuids, ( std::vector<std::string>{ kMaterial } ) );
    EXPECT_FALSE( cubes.CastShadows.has_value() );
    ASSERT_TRUE( cubes.InstanceTransforms.has_value() );
    ASSERT_EQ( cubes.InstanceTransforms->size(), 5u );
    // Member order: the parent's composite (the child), cube A, cube B, then the field's two, verbatim.
    // The child sits 100 along its parent's +X, and the parent is turned a quarter about +Y: world -Z.
    const glm::vec3 child = TranslationOf( ( *cubes.InstanceTransforms )[0] );
    const glm::vec3 want  = CellCentre( 1, 1 ) + glm::vec3( 0.0f, 0.0f, -100.0f );
    EXPECT_NEAR( child.x, want.x, 1e-2f );
    EXPECT_NEAR( child.z, want.z, 1e-2f );
    EXPECT_EQ( TranslationOf( ( *cubes.InstanceTransforms )[3] ), kFieldA );
    EXPECT_EQ( TranslationOf( ( *cubes.InstanceTransforms )[4] ), kFieldB );

    EXPECT_EQ( batches[1].Primitive, Desert::Geometry::PrimitiveType::Sphere );
    EXPECT_EQ( batches[1].InstanceTransforms->size(), 1u );
    EXPECT_EQ( batches[2].CastShadows, std::optional<bool>( false ) );
    EXPECT_EQ( batches[2].InstanceTransforms->size(), 1u );

    std::vector<std::uint64_t> sources = hlod->Sources;
    std::sort( sources.begin(), sources.end() );
    EXPECT_EQ( sources, ( std::vector<std::uint64_t>{ kChild, kCubeA, kCubeB, kSphere, kNoShadow, kField } ) );
}

TEST( WorldCellsHLOD, WhatDrawsAndCannotBeBatchedIsNamedWithItsReason )
{
    const Cooked cooked = CookIt( World() );
    const auto*  hlod   = HLODHolding( cooked.Index, kCubeA );
    ASSERT_NE( hlod, nullptr );
    std::map<std::uint64_t, Rules::HLODExclusion> missing;
    for ( const auto& row : hlod->NotInstanced )
        missing[row.Id] = row.Reason;
    EXPECT_EQ( missing, ( std::map<std::uint64_t, Rules::HLODExclusion>{
                             { kHidden, Rules::HLODExclusion::HiddenSubmeshes },
                             { kSkinned, Rules::HLODExclusion::SkinnedMesh },
                             { kEdited, Rules::HLODExclusion::EditorMesh } } ) );

    // The index spells the reason as a name of the closed list.
    const std::string text = Common::Json::Write( cooked.Index );
    EXPECT_NE( text.find( R"("Reason":"HiddenSubmeshes")" ), std::string::npos ) << text;

    // The prefab instance's cell has nothing to batch: a row without a file, naming the hole.
    const auto* origin = HLODHolding( cooked.Index, kPrefab );
    ASSERT_NE( origin, nullptr );
    EXPECT_FALSE( origin->File.has_value() );
    EXPECT_TRUE( origin->Ids.empty() );
    ASSERT_EQ( origin->NotInstanced.size(), 1u );
    EXPECT_EQ( origin->NotInstanced[0].Reason, Rules::HLODExclusion::PrefabInstance );
    auto none = Cells::HLODRecords( cooked.Index, ReaderOf( cooked.Files ),
                                    static_cast<std::size_t>( origin - cooked.Index.HLODs.data() ) );
    ASSERT_TRUE( none.IsSuccess() ) << none.GetError();
    EXPECT_TRUE( none.GetValue().empty() );

    // A cell that draws nothing has no HLOD; one far cube has its own.
    EXPECT_EQ( HLODHolding( cooked.Index, kEmptyCell ), nullptr );
    const auto* distant = HLODHolding( cooked.Index, kFar );
    ASSERT_NE( distant, nullptr );
    EXPECT_EQ( distant->Instances, 1u );
    EXPECT_EQ( cooked.Index.HLODs.size(), 3u );
}

TEST( WorldCellsHLOD, TheHLODIsNotPartOfTheWorld )
{
    const SceneSerialized source = World();
    const Cooked          cooked = CookIt( source );
    EXPECT_EQ( cooked.Index.Records, source.Entities.size() );
    auto back = Cells::AssembleWorld( cooked.Index, ReaderOf( cooked.Files ) );
    ASSERT_TRUE( back.IsSuccess() ) << back.GetError();
    EXPECT_EQ( Cells::CanonicalRecords( back.GetValue() ), Cells::CanonicalRecords( source ) );

    // Two cooks, same bytes, HLOD files included.
    const Cooked again = CookIt( source );
    EXPECT_EQ( cooked.Files, again.Files );
}

TEST( WorldCellsHLOD, ADamagedHLODFileIsRefusedByName )
{
    Cooked      cooked = CookIt( World() );
    const auto* hlod   = HLODHolding( cooked.Index, kCubeA );
    ASSERT_NE( hlod, nullptr );
    ASSERT_TRUE( hlod->File.has_value() );
    auto& bytes = cooked.Files.at( *hlod->File );
    bytes[bytes.size() - 2] ^= 0x01;
    auto records = Cells::HLODRecords( cooked.Index, ReaderOf( cooked.Files ),
                                       static_cast<std::size_t>( hlod - cooked.Index.HLODs.data() ) );
    ASSERT_FALSE( records.IsSuccess() );
    EXPECT_NE( records.GetError().find( *hlod->File ), std::string::npos ) << records.GetError();
}

TEST( WorldCellsHLOD, AnHLODStandingInForAnAlwaysLoadedUnitIsRefused )
{
    Cooked cooked = CookIt( World() );
    ASSERT_FALSE( cooked.Index.HLODs.empty() );
    ASSERT_TRUE( cooked.Index.Units.front().Reason.has_value() ); // the camera's
    cooked.Index.HLODs.front().Unit = 0;

    // Rewritten through the envelope writer, so every checksum is true and only the payload differs.
    auto&                      bytes = cooked.Files.at( std::string( Cells::kIndexFileName ) );
    const CC::SubsystemVersion known[] = { { Cells::kWorldFormatTag, Cells::kWorldFormatVersion } };
    auto envelope = CC::ReadAssetEnvelope( std::as_bytes( std::span( bytes ) ), { known } );
    ASSERT_TRUE( envelope ) << envelope.GetError();
    CC::AssetEnvelope edited  = envelope.ExtractValue();
    const std::string payload = Common::Json::Write( cooked.Index );
    for ( auto& section : edited.Sections )
        if ( section.Tag == CC::EnvelopeSection::Payload )
        {
            section.Bytes.resize( payload.size() );
            std::memcpy( section.Bytes.data(), payload.data(), payload.size() );
        }
    auto rewritten = CC::WriteAssetEnvelope( edited );
    ASSERT_TRUE( rewritten ) << rewritten.GetError();
    const auto*                      data = reinterpret_cast<const unsigned char*>( rewritten.GetValue().data() );
    const std::vector<unsigned char> changed( data, data + rewritten.GetValue().size() );

    const auto read = Cells::ReadWorldIndex( Cells::kIndexFileName, changed );
    ASSERT_FALSE( read.IsSuccess() );
    EXPECT_NE( read.GetError().find( Cells::kIndexFileName ), std::string::npos ) << read.GetError();
    EXPECT_NE( read.GetError().find( "not a cell" ), std::string::npos ) << read.GetError();
}
