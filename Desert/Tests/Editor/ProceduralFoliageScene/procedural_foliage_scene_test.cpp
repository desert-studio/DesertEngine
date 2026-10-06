// S1: Resimulate of a procedural foliage volume over a world of foliage fields — the real simulation, trace
// filter, type rules and FO-6 cell filing (ProceduralFoliageResimulate.cpp), with the Scene replaced by an
// in-memory world whose ground is a flat landscape at height 0.

#include <Editor/Panels/ViewportPanel/Tools/ProceduralFoliageResimulate.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <tuple>
#include <vector>

namespace
{
    using namespace Desert;
    using Assets::Serialization::FoliageTypeData;
    namespace Procedural = World::Foliage::Procedural;

    FoliageTypeData Type( float radius, float density )
    {
        FoliageTypeData type;
        type.Procedural.CollisionRadius       = radius;
        type.Procedural.ShadeRadius           = radius;
        type.Procedural.InitialSeedDensity    = density;
        type.Procedural.ProceduralScale       = { 1.0f, 1.0f };
        type.Procedural.AverageSpreadDistance = 40.0f;
        type.Procedural.SpreadVariance        = 60.0f;
        return type;
    }

    struct Field
    {
        Common::UUID              Owner = Common::UUID::Null();
        uint32_t                  Type  = 0;
        World::Foliage::CellCoord Cell;
        std::vector<glm::mat4>    Instances;
        int                       Id = 0;
    };

    // The world: fields, and a landscape plane at y = 0 (a static mesh nowhere).
    struct FakeWorld
    {
        std::vector<Field> Fields;
        int                NextId = 1;

        Common::ResultStr<Editor::Tools::ProceduralFoliageResimulated>
        Resimulate( const ECS::ProceduralFoliageData& volume, const Common::UUID& owner,
                    const std::vector<FoliageTypeData>&                 types,
                    Editor::Tools::ProceduralFoliageTransaction<Field>* transaction = nullptr )
        {
            Editor::Tools::ProceduralFoliageHost host;
            host.CellSize = 2000.0;
            host.Trace    = []( const glm::vec3& start, const glm::vec3& end,
                             const Editor::Tools::FoliageSurfaceFilter& filter )
                 -> std::optional<Editor::Tools::FoliageTraceHit>
            {
                if ( !filter.Landscape || start.y < 0.0f || end.y > 0.0f )
                    return std::nullopt;
                const float                    t = start.y / ( start.y - end.y );
                Editor::Tools::FoliageTraceHit hit;
                hit.Point   = start + ( end - start ) * t;
                hit.Surface = Editor::Tools::FoliageSurface::Landscape;
                return hit;
            };
            std::vector<size_t> live;
            for ( size_t i = 0; i < Fields.size(); ++i )
            {
                host.Existing.push_back( { Fields[i].Owner, Fields[i].Type, Fields[i].Cell } );
                live.push_back( i );
            }
            std::vector<bool> removed( Fields.size(), false );
            const auto        touch = [&]( size_t e )
            {
                if ( transaction )
                    transaction->Touch( IdOf( Fields[live[e]] ) );
            };
            host.Rewrite = [&]( size_t e, std::vector<glm::mat4> instances )
            {
                touch( e );
                Fields[live[e]].Instances = std::move( instances );
            };
            host.Remove = [&]( size_t e )
            {
                touch( e );
                removed[live[e]] = true;
            };
            std::vector<Field> created;
            host.Create = [&]( const Procedural::ProceduralFoliageTypeField& fresh ) -> Common::BoolResultStr
            {
                created.push_back( { owner, fresh.TypeIndex, fresh.Cell, fresh.Instances, NextId++ } );
                if ( transaction )
                    transaction->Made( IdOf( created.back() ) );
                return BOOLSUCCESS;
            };
            auto done =
                 Editor::Tools::ResimulateProceduralFoliage( volume, glm::vec3( 0.0f ), owner, types, host );
            std::vector<Field> kept;
            for ( size_t i = 0; i < Fields.size(); ++i )
                if ( !removed[i] )
                    kept.push_back( std::move( Fields[i] ) );
            for ( auto& f : created )
                kept.push_back( std::move( f ) );
            Fields = std::move( kept );
            if ( transaction )
                transaction->Close();
            return done;
        }

        static Common::UUID IdOf( const Field& field )
        {
            return Common::UUID( static_cast<uint64_t>( field.Id ) );
        }

        // The store a Resimulate's undo step reads and writes this world through.
        Editor::Tools::ProceduralFoliageFieldStore<Field> Store()
        {
            return { .Capture = [this]( const Common::UUID& id ) -> std::optional<Field>
                     {
                         for ( const auto& f : Fields )
                             if ( IdOf( f ) == id )
                                 return f;
                         return std::nullopt;
                     },
                     .Destroy = [this]( const Common::UUID& id )
                     { std::erase_if( Fields, [&]( const Field& f ) { return IdOf( f ) == id; } ); },
                     .Restore =
                          [this]( const Field& field )
                     {
                         Fields.push_back( field );
                         return true;
                     } };
        }

        // The fields in Id order, as a comparable list (an undo may bring a field back at another position).
        std::vector<std::tuple<int, Common::UUID, uint32_t, int32_t, int32_t, std::vector<glm::mat4>>>
        State() const
        {
            std::vector<std::tuple<int, Common::UUID, uint32_t, int32_t, int32_t, std::vector<glm::mat4>>> out;
            for ( const auto& f : Fields )
                out.emplace_back( f.Id, f.Owner, f.Type, f.Cell.X, f.Cell.Z, f.Instances );
            std::sort( out.begin(), out.end(),
                       []( const auto& a, const auto& b ) { return std::get<0>( a ) < std::get<0>( b ); } );
            return out;
        }
    };

    ECS::ProceduralFoliageData Volume()
    {
        ECS::ProceduralFoliageData v;
        v.Extent         = glm::vec3( 3000.0f, 1000.0f, 3000.0f );
        v.TileSize       = 2000.0f;
        v.NumUniqueTiles = 2;
        return v;
    }
} // namespace

TEST( ProceduralFoliageScene, ResimulateFillsOwnedCellFieldsOnTheLandscapeInsideTheVolume )
{
    FakeWorld  world;
    const auto owner = Common::UUID( 77u );
    auto       done  = world.Resimulate( Volume(), owner, { Type( 60.0f, 2.0f ) } );
    ASSERT_TRUE( done ) << done.GetError();
    EXPECT_GT( done.GetValue().Instances, 0u );
    EXPECT_GT( world.Fields.size(), 1u ) << "a 60 m box over 20 m cells files into several cell fields";
    for ( const auto& f : world.Fields )
    {
        EXPECT_EQ( f.Owner, owner );
        for ( const auto& m : f.Instances )
        {
            const glm::vec3 p( m[3] );
            EXPECT_NEAR( p.y, 0.0f, 1e-3f ) << "on the landscape";
            EXPECT_LE( std::abs( p.x ), 3000.0f );
            EXPECT_LE( std::abs( p.z ), 3000.0f );
            EXPECT_EQ( World::Foliage::FoliageCellOf( m, 2000.0 ), f.Cell ) << "filed in its own cell";
        }
    }
}

TEST( ProceduralFoliageScene, AResimulationKeepsPaintedAndForeignFieldsAndRewritesItsOwnInPlace )
{
    FakeWorld world;
    world.Fields.push_back( { Common::UUID::Null(), 0, { 0, 0 }, { glm::mat4( 1.0f ) }, world.NextId++ } );
    world.Fields.push_back( { Common::UUID( 5u ), 0, { 0, 0 }, { glm::mat4( 2.0f ) }, world.NextId++ } );
    const auto                         owner = Common::UUID( 77u );
    const std::vector<FoliageTypeData> types{ Type( 60.0f, 2.0f ) };
    ASSERT_TRUE( world.Resimulate( Volume(), owner, types ) );
    const auto first = world.Fields;

    auto again = world.Resimulate( Volume(), owner, types );
    ASSERT_TRUE( again ) << again.GetError();
    EXPECT_EQ( again.GetValue().Created, 0u );
    EXPECT_EQ( again.GetValue().Removed, 0u );
    ASSERT_EQ( world.Fields.size(), first.size() );
    for ( size_t i = 0; i < first.size(); ++i )
    {
        EXPECT_EQ( world.Fields[i].Id, first[i].Id ) << "the same fields keep their identity";
        EXPECT_EQ( world.Fields[i].Instances, first[i].Instances ) << "and the same instances";
    }
    EXPECT_EQ( world.Fields[0].Instances, std::vector<glm::mat4>{ glm::mat4( 1.0f ) } ) << "painted untouched";
    EXPECT_EQ( world.Fields[1].Instances, std::vector<glm::mat4>{ glm::mat4( 2.0f ) } ) << "foreign untouched";

    // A volume that may not land on a landscape grows nothing here: its own fields go, the others stay.
    auto noLandscape           = Volume();
    noLandscape.AllowLandscape = false;
    auto emptied               = world.Resimulate( noLandscape, owner, types );
    ASSERT_TRUE( emptied ) << emptied.GetError();
    EXPECT_EQ( emptied.GetValue().Instances, 0u );
    ASSERT_EQ( world.Fields.size(), 2u );
    EXPECT_EQ( world.Fields[0].Owner, Common::UUID::Null() );
    EXPECT_EQ( world.Fields[1].Owner, Common::UUID( 5u ) );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

TEST( ProceduralFoliageScene, ResimulateIsOneUndoStepThatPutsTheFieldsBackAsTheyWere )
{
    FakeWorld world;
    world.Fields.push_back( { Common::UUID::Null(), 0, { 0, 0 }, { glm::mat4( 1.0f ) }, world.NextId++ } );
    const auto                         owner = Common::UUID( 77u );
    const std::vector<FoliageTypeData> types{ Type( 60.0f, 2.0f ) };
    const auto                         painted = world.State();

    // Created fields: Undo takes them away, Redo brings the same ones back.
    Editor::Tools::ProceduralFoliageTransaction<Field> grow( world.Store() );
    ASSERT_TRUE( world.Resimulate( Volume(), owner, types, &grow ) );
    const auto grown = world.State();
    ASSERT_GT( grown.size(), painted.size() );
    ASSERT_FALSE( grow.Empty() );
    EXPECT_TRUE( grow.Undo() );
    EXPECT_EQ( world.State(), painted ) << "undo leaves the painted field alone and no generated one";
    EXPECT_TRUE( grow.Redo() );
    EXPECT_EQ( world.State(), grown ) << "redo brings the same fields, ids and instances";

    // Rewritten and removed fields: a resimulation that grows nothing removes the volume's fields; Undo
    // returns them under their own ids with their instances, Redo removes them again.
    auto noLandscape           = Volume();
    noLandscape.AllowLandscape = false;
    Editor::Tools::ProceduralFoliageTransaction<Field> clear( world.Store() );
    ASSERT_TRUE( world.Resimulate( noLandscape, owner, types, &clear ) );
    const auto cleared = world.State();
    ASSERT_EQ( cleared, painted );
    EXPECT_TRUE( clear.Undo() );
    EXPECT_EQ( world.State(), grown );
    EXPECT_TRUE( clear.Redo() );
    EXPECT_EQ( world.State(), cleared );
}
