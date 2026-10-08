#include <Engine/Destruction/DestructionWorld.hpp>

#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <format>
#include <utility>

namespace Desert::Destruction
{
    namespace
    {
        // Two leaves are connected when no face plane of either hull has the other hull farther than this
        // outside it. Neighbouring cells share a wall exactly; a hull of a non-convex island bulges over it.
        // Edge-edge axes are not tested, so two hulls meeting near a corner count as connected: a break
        // then leaves one more piece together, never splits two that touch.
        constexpr float kNeighbourToleranceCm = 0.5f;

        struct HullPlanes
        {
            std::vector<glm::vec3> Normals;
            std::vector<float>     Offsets;
            glm::vec3              Min{ 0.0f };
            glm::vec3              Max{ 0.0f };
        };

        HullPlanes PlanesOf( const FractureNode& leaf )
        {
            HullPlanes planes;
            planes.Min = planes.Max = leaf.HullVertices.front();
            for ( const glm::vec3& v : leaf.HullVertices )
            {
                planes.Min = glm::min( planes.Min, v );
                planes.Max = glm::max( planes.Max, v );
            }
            for ( const std::vector<int>& face : leaf.HullFaces )
            {
                if ( face.size() < 3u )
                    continue;
                const glm::vec3& a = leaf.HullVertices[face[0]];
                const glm::vec3  n = glm::cross( leaf.HullVertices[face[1]] - a, leaf.HullVertices[face[2]] - a );
                const float      length = glm::length( n );
                if ( !( length > 0.0f ) )
                    continue;
                planes.Normals.push_back( n / length );
                planes.Offsets.push_back( glm::dot( n / length, a ) );
            }
            return planes;
        }

        bool Separated( const HullPlanes& planes, const std::vector<glm::vec3>& other )
        {
            for ( size_t i = 0; i < planes.Normals.size(); ++i )
            {
                float nearest = std::numeric_limits<float>::max();
                for ( const glm::vec3& v : other )
                    nearest = std::min( nearest, glm::dot( planes.Normals[i], v ) - planes.Offsets[i] );
                if ( nearest > kNeighbourToleranceCm )
                    return true;
            }
            return false;
        }

        bool Touch( const FractureNode& a, const HullPlanes& pa, const FractureNode& b, const HullPlanes& pb )
        {
            const glm::vec3 grow( kNeighbourToleranceCm );
            if ( glm::any( glm::greaterThan( pa.Min - grow, pb.Max ) ) ||
                 glm::any( glm::greaterThan( pb.Min - grow, pa.Max ) ) )
                return false;
            return !Separated( pa, b.HullVertices ) && !Separated( pb, a.HullVertices );
        }

        // A fraction in [0, 1) from a node index: the same body sleeps as long on every run and platform.
        float SleepFraction( int32_t node )
        {
            uint32_t h = static_cast<uint32_t>( node ) * 2654435761u;
            h ^= h >> 16u;
            h *= 2246822519u;
            h ^= h >> 13u;
            return static_cast<float>( h >> 8u ) / static_cast<float>( 1u << 24u );
        }
    } // namespace

    DestructionWorld::DestructionWorld( Physics::PhysicsWorld& physics ) : m_Physics( physics )
    {
        m_Physics.SetStepCallback( [this]( float dt ) { Advance( dt ); } );
    }

    DestructionWorld::~DestructionWorld()
    {
        m_Physics.SetStepCallback( {} );
        for ( uint32_t i = 0; i < m_Objects.size(); ++i )
            Remove( i );
    }

    Common::ResultStr<DestructibleHandle> DestructionWorld::Add( std::shared_ptr<const FractureData> data,
                                                                 const DestructibleDesc&             desc )
    {
        using Result = DestructibleHandle;
        if ( !data || data->Nodes.empty() )
            return Common::MakeError<Result>( "a destructible needs a fracture with at least its root node" );
        const DestructionSettings& settings = desc.Settings;
        if ( !( settings.DensityKgPerCm3 > 0.0f ) )
            return Common::MakeError<Result>(
                 std::format( "density {} kg/cm^3 is not positive", settings.DensityKgPerCm3 ) );
        if ( !( settings.MaxSleepTime.x >= 0.0f ) || settings.MaxSleepTime.y < settings.MaxSleepTime.x )
            return Common::MakeError<Result>( std::format( "sleep time range [{}, {}] s is not a range",
                                                           settings.MaxSleepTime.x, settings.MaxSleepTime.y ) );

        const std::vector<FractureNode>& nodes = data->Nodes;
        const auto                       count = static_cast<int32_t>( nodes.size() );
        if ( nodes[0].Parent != -1 )
            return Common::MakeError<Result>(
                 std::format( "node 0 has parent {}: it must be the root", nodes[0].Parent ) );

        Object object;
        object.Settings = settings;
        object.Nodes.resize( nodes.size() );
        for ( int32_t i = 1; i < count; ++i )
        {
            const int32_t parent = nodes[i].Parent;
            if ( parent < 0 || parent >= i )
                return Common::MakeError<Result>(
                     std::format( "node {} has parent {}: parents come before their children", i, parent ) );
            object.Nodes[i].Parent = parent;
            object.Nodes[parent].Children.push_back( i );
        }
        // Leaves bottom-up: children come after parents, so a reverse walk sees every child first.
        for ( int32_t i = count - 1; i >= 0; --i )
        {
            NodeState&     node  = object.Nodes[i];
            const uint32_t level = nodes[i].Level;
            node.InternalStrain =
                 level < desc.DamageThreshold.size() ? desc.DamageThreshold[level] : nodes[i].DamageThreshold;
            if ( node.Children.empty() )
            {
                if ( nodes[i].HullVertices.empty() )
                    return Common::MakeError<Result>( std::format( "leaf node {} has no hull", i ) );
                node.Leaves.push_back( i );
            }
            for ( const int32_t child : node.Children )
                node.Leaves.insert( node.Leaves.end(), object.Nodes[child].Leaves.begin(),
                                    object.Nodes[child].Leaves.end() );
        }
        for ( const int32_t anchored : desc.AnchoredNodes )
        {
            if ( anchored < 0 || anchored >= count )
                return Common::MakeError<Result>(
                     std::format( "anchored node {} is not one of the fracture's {} nodes", anchored, count ) );
            for ( const int32_t leaf : object.Nodes[anchored].Leaves )
                for ( int32_t up = leaf; up >= 0; up = object.Nodes[up].Parent )
                    object.Nodes[up].Anchored = true;
        }

        // Connectivity between leaves (UE's proximity of a geometry collection, made at fracture time there).
        const std::vector<int32_t>& leaves = object.Nodes[0].Leaves;
        std::vector<HullPlanes>     planes;
        planes.reserve( leaves.size() );
        for ( const int32_t leaf : leaves )
            planes.push_back( PlanesOf( nodes[leaf] ) );
        for ( size_t a = 0; a < leaves.size(); ++a )
            for ( size_t b = a + 1u; b < leaves.size(); ++b )
                if ( Touch( nodes[leaves[a]], planes[a], nodes[leaves[b]], planes[b] ) )
                {
                    object.Nodes[leaves[a]].Neighbours.push_back( leaves[b] );
                    object.Nodes[leaves[b]].Neighbours.push_back( leaves[a] );
                }

        object.Data            = std::move( data );
        const auto objectIndex = static_cast<uint32_t>( m_Objects.size() );
        m_Objects.push_back( std::move( object ) );

        auto spawned = SpawnBody( objectIndex, { 0 }, desc.Position, desc.Rotation, glm::vec3( 0.0f ),
                                  glm::vec3( 0.0f ), false );
        if ( !spawned.IsSuccess() )
        {
            m_Objects.pop_back();
            return Common::MakeError<Result>( spawned.GetError() );
        }
        return Common::MakeSuccess( static_cast<DestructibleHandle>( objectIndex ) );
    }

    void DestructionWorld::Remove( DestructibleHandle object )
    {
        if ( object >= m_Objects.size() || !m_Objects[object].Data )
            return;
        for ( uint32_t i = 0; i < m_Objects[object].Bodies.size(); ++i )
            DestroyBody( object, i );
        m_Objects[object] = Object{};
    }

    Physics::BodyHandle DestructionWorld::GetNodeBody( DestructibleHandle object, int32_t node ) const
    {
        if ( object >= m_Objects.size() || !m_Objects[object].Data )
            return Physics::kInvalidBody;
        const Object& o = m_Objects[object];
        if ( node < 0 || node >= static_cast<int32_t>( o.Nodes.size() ) || o.Nodes[node].Body < 0 )
            return Physics::kInvalidBody;
        return o.Bodies[o.Nodes[node].Body].Handle;
    }

    uint32_t DestructionWorld::GetBodyCount( DestructibleHandle object ) const
    {
        if ( object >= m_Objects.size() )
            return 0u;
        return static_cast<uint32_t>( std::count_if( m_Objects[object].Bodies.begin(),
                                                     m_Objects[object].Bodies.end(), []( const BodyState& b )
                                                     { return b.Handle != Physics::kInvalidBody; } ) );
    }

    std::vector<int32_t> DestructionWorld::UnitsOf( const Object& object, const BodyState& body )
    {
        if ( body.Members.size() == 1u )
            return object.Nodes[body.Members[0]].Children;
        return body.Members;
    }

    int32_t DestructionWorld::UnitOfLeaf( const Object& object, const BodyState& body, int32_t leaf )
    {
        const int32_t unitParent =
             body.Members.size() == 1u ? body.Members[0] : object.Nodes[body.Members[0]].Parent;
        if ( leaf == unitParent )
            return -1; // a body of one leaf has no units
        int32_t unit = leaf;
        while ( unit >= 0 && object.Nodes[unit].Parent != unitParent )
            unit = object.Nodes[unit].Parent;
        return unit;
    }

    glm::dvec3 DestructionWorld::CenterOfMass( const Object& object, const std::vector<int32_t>& members )
    {
        glm::dvec3 sum( 0.0 );
        double     volume = 0.0;
        for ( const int32_t m : members )
        {
            const FractureNode& node = object.Data->Nodes[m];
            sum += node.CenterOfMass * node.Volume;
            volume += node.Volume;
        }
        return volume > 0.0 ? sum / volume : object.Data->Nodes[members[0]].CenterOfMass;
    }

    void DestructionWorld::AssignBody( Object& object, int32_t node, int32_t body )
    {
        std::vector<int32_t> pending{ node };
        while ( !pending.empty() )
        {
            const int32_t current = pending.back();
            pending.pop_back();
            object.Nodes[current].Body       = body;
            const std::vector<int32_t>& kids = object.Nodes[current].Children;
            pending.insert( pending.end(), kids.begin(), kids.end() );
        }
    }

    Common::ResultStr<uint32_t> DestructionWorld::SpawnBody( uint32_t objectIndex, std::vector<int32_t> members,
                                                             const glm::vec3& position, const glm::quat& rotation,
                                                             const glm::vec3& linear, const glm::vec3& angular,
                                                             bool broken )
    {
        Object&                          object = m_Objects[objectIndex];
        const std::vector<FractureNode>& nodes  = object.Data->Nodes;

        BodyState body;
        body.Broken                                    = broken;
        double                                  volume = 0.0;
        std::vector<std::span<const glm::vec3>> parts;
        for ( const int32_t m : members )
        {
            volume += nodes[m].Volume;
            body.Static = body.Static || object.Nodes[m].Anchored;
            for ( const int32_t leaf : object.Nodes[m].Leaves )
            {
                body.PartLeaf.push_back( leaf );
                parts.emplace_back( nodes[leaf].HullVertices );
            }
        }
        body.Members                        = std::move( members );
        const DestructionSettings& settings = object.Settings;
        body.MaxSleep                       = settings.MaxSleepTime.x +
                        ( settings.MaxSleepTime.y - settings.MaxSleepTime.x ) * SleepFraction( body.Members[0] );

        Physics::CompoundBodyDesc desc;
        desc.Parts                 = parts;
        desc.Type                  = body.Static ? Physics::BodyType::Static : Physics::BodyType::Dynamic;
        desc.Mass                  = static_cast<float>( volume ) * settings.DensityKgPerCm3;
        desc.Friction              = settings.Friction;
        desc.Restitution           = settings.Restitution;
        desc.Position              = position;
        desc.Rotation              = rotation;
        desc.LinearVelocity        = linear;
        desc.AngularVelocity       = angular;
        desc.ReportContactImpulses = true;
        auto created               = m_Physics.CreateCompoundBody( desc );
        if ( !created.IsSuccess() )
            return Common::MakeError<uint32_t>( created.GetError() );
        body.Handle = created.GetValue();

        uint32_t slot = 0u;
        while ( slot < object.Bodies.size() && object.Bodies[slot].Handle != Physics::kInvalidBody )
            ++slot;
        if ( slot == object.Bodies.size() )
            object.Bodies.emplace_back();
        for ( const int32_t m : body.Members )
            AssignBody( object, m, static_cast<int32_t>( slot ) );
        m_BodyLookup[body.Handle] = BodyRef{ objectIndex, slot };
        object.Bodies[slot]       = std::move( body );
        return Common::MakeSuccess( slot );
    }

    void DestructionWorld::DestroyBody( uint32_t objectIndex, uint32_t bodyIndex )
    {
        Object&    object = m_Objects[objectIndex];
        BodyState& body   = object.Bodies[bodyIndex];
        if ( body.Handle == Physics::kInvalidBody )
            return;
        for ( const int32_t m : body.Members )
            AssignBody( object, m, -1 );
        m_BodyLookup.erase( body.Handle );
        m_Physics.RemoveBody( body.Handle );
        body = BodyState{};
    }

    void DestructionWorld::Advance( float dt )
    {
        // ComputeStrainFromCollision: every contact strains the unit holding the part it touches.
        std::vector<BodyRef>                      strained;
        std::vector<std::pair<uint32_t, int32_t>> touched; // (object, unit) to reset after the step
        const auto strain = [&]( Physics::BodyHandle handle, uint32_t part, float impulse )
        {
            const auto found = m_BodyLookup.find( handle );
            if ( found == m_BodyLookup.end() )
                return;
            Object&          object = m_Objects[found->second.Object];
            const BodyState& body   = object.Bodies[found->second.Body];
            if ( part >= body.PartLeaf.size() )
                return;
            const int32_t unit = UnitOfLeaf( object, body, body.PartLeaf[part] );
            if ( unit < 0 )
                return;
            object.Nodes[unit].CollisionImpulse += impulse;
            touched.emplace_back( found->second.Object, unit );
            strained.push_back( found->second );
        };
        for ( const Physics::ContactImpulse& contact : m_Physics.GetStepContactImpulses() )
        {
            if ( !( contact.Impulse > 0.0f ) || !std::isfinite( contact.Impulse ) )
                continue;
            strain( contact.Body1, contact.Part1, contact.Impulse );
            strain( contact.Body2, contact.Part2, contact.Impulse );
        }

        // A unit whose strain is zero or less breaks without being touched (AdvanceClustering :1730).
        for ( uint32_t o = 0; o < m_Objects.size(); ++o )
            for ( uint32_t b = 0; b < m_Objects[o].Bodies.size(); ++b )
                if ( m_Objects[o].Bodies[b].Handle != Physics::kInvalidBody )
                    for ( const int32_t unit : UnitsOf( m_Objects[o], m_Objects[o].Bodies[b] ) )
                        if ( m_Objects[o].Nodes[unit].InternalStrain <= 0.0f )
                        {
                            strained.push_back( BodyRef{ o, b } );
                            break;
                        }

        Release( std::move( strained ) );
        for ( const auto& [o, unit] : touched )
            m_Objects[o].Nodes[unit].CollisionImpulse = 0.0f; // ResetCollisionImpulseArray

        // Remove on Sleep.
        for ( uint32_t o = 0; o < m_Objects.size(); ++o )
        {
            Object& object = m_Objects[o];
            if ( !object.Data || !object.Settings.RemoveOnSleep )
                continue;
            for ( uint32_t b = 0; b < object.Bodies.size(); ++b )
            {
                BodyState& body = object.Bodies[b];
                if ( body.Handle == Physics::kInvalidBody || !body.Broken || body.Static )
                    continue;
                const bool slow = object.Settings.SlowMovingAsSleeping &&
                                  glm::length( m_Physics.GetLinearVelocity( body.Handle ) ) <
                                       object.Settings.SlowMovingVelocityThreshold;
                body.SleepTime = ( !m_Physics.IsActive( body.Handle ) || slow ) ? body.SleepTime + dt : 0.0f;
                if ( body.SleepTime < body.MaxSleep )
                    continue;
                const glm::vec3 position = m_Physics.GetPosition( body.Handle );
                const glm::quat rotation = m_Physics.GetRotation( body.Handle );
                for ( const int32_t m : body.Members )
                    m_Events.push_back(
                         DestructionEvent{ DestructionEventKind::Removed, o, m,
                                           position + rotation * glm::vec3( object.Data->Nodes[m].CenterOfMass ),
                                           glm::vec3( 0.0f ) } );
                DestroyBody( o, b );
            }
        }
    }

    void DestructionWorld::Release( std::vector<BodyRef> strained )
    {
        // BreakingModel: each strained body once, in a fixed order.
        std::sort( strained.begin(), strained.end(), []( const BodyRef& a, const BodyRef& b )
                   { return a.Object != b.Object ? a.Object < b.Object : a.Body < b.Body; } );
        strained.erase( std::unique( strained.begin(), strained.end(), []( const BodyRef& a, const BodyRef& b )
                                     { return a.Object == b.Object && a.Body == b.Body; } ),
                        strained.end() );
        for ( const BodyRef& ref : strained )
        {
            const Object& object = m_Objects[ref.Object];
            if ( object.Bodies[ref.Body].Handle == Physics::kInvalidBody )
                continue;
            std::vector<int32_t> released;
            for ( const int32_t unit : UnitsOf( object, object.Bodies[ref.Body] ) )
            {
                const NodeState& n = object.Nodes[unit];
                if ( std::max( n.CollisionImpulse, n.ExternalStrain ) >= n.InternalStrain ) // :1178
                    released.push_back( unit );
            }
            if ( !released.empty() )
                Break( ref.Object, ref.Body, released );
        }
    }

    glm::vec3 DestructionWorld::WorldPoint( const BodyState& body, const glm::dvec3& local ) const
    {
        return m_Physics.GetPosition( body.Handle ) + m_Physics.GetRotation( body.Handle ) * glm::vec3( local );
    }

    uint32_t DestructionWorld::ApplyField( const FieldCommand& command )
    {
        uint32_t acted = 0u;
        // Bodies alive now, by reference: a break or a kill below changes the slots.
        std::vector<BodyRef> bodies;
        for ( uint32_t o = 0; o < m_Objects.size(); ++o )
            for ( uint32_t b = 0; b < m_Objects[o].Bodies.size(); ++b )
                if ( m_Objects[o].Data && m_Objects[o].Bodies[b].Handle != Physics::kInvalidBody )
                    bodies.push_back( BodyRef{ o, b } );

        switch ( command.Type )
        {
            case FieldPhysicsType::Impulse:
            case FieldPhysicsType::ExternalStrain:
            {
                // ExternalClusterStrain at every unit's centre of mass, then the release of this instant.
                const bool           impulse = command.Type == FieldPhysicsType::Impulse;
                std::vector<BodyRef> strained;
                for ( const BodyRef& ref : bodies )
                {
                    Object&          object = m_Objects[ref.Object];
                    const BodyState& body   = object.Bodies[ref.Body];
                    for ( const int32_t unit : UnitsOf( object, body ) )
                    {
                        const glm::vec3 at = WorldPoint( body, object.Data->Nodes[unit].CenterOfMass );
                        const float     s  = impulse ? glm::length( Evaluate( command.Vector, at ) )
                                                     : Evaluate( command.Scalar, at );
                        if ( !( s > 0.0f ) || !std::isfinite( s ) )
                            continue;
                        object.Nodes[unit].ExternalStrain = std::max( object.Nodes[unit].ExternalStrain, s );
                        strained.push_back( ref );
                    }
                }
                Release( strained );
                for ( Object& object : m_Objects )
                    for ( NodeState& node : object.Nodes )
                        node.ExternalStrain = 0.0f; // UE resets it with the release (:1247)
                acted = static_cast<uint32_t>( strained.size() );
                if ( !impulse )
                    break;
                // The impulse then acts on the bodies the break left (the pieces, not the whole that was).
                acted = 0u;
                for ( const Object& obj : m_Objects )
                    for ( const BodyState& body : obj.Bodies )
                    {
                        if ( body.Handle == Physics::kInvalidBody || body.Static )
                            continue;
                        const glm::vec3 j =
                             Evaluate( command.Vector, WorldPoint( body, CenterOfMass( obj, body.Members ) ) );
                        if ( !( glm::length( j ) > 0.0f ) )
                            continue;
                        m_Physics.AddImpulse( body.Handle, j );
                        ++acted;
                    }
                break;
            }
            case FieldPhysicsType::Kill:
                for ( const BodyRef& ref : bodies )
                {
                    Object&          object = m_Objects[ref.Object];
                    const BodyState& body   = object.Bodies[ref.Body];
                    if ( !( Evaluate( command.Scalar, WorldPoint( body, CenterOfMass( object, body.Members ) ) ) >
                            0.0f ) )
                        continue;
                    for ( const int32_t m : body.Members )
                        m_Events.push_back(
                             DestructionEvent{ DestructionEventKind::Removed, ref.Object, m,
                                               WorldPoint( body, object.Data->Nodes[m].CenterOfMass ),
                                               m_Physics.GetLinearVelocity( body.Handle ) } );
                    DestroyBody( ref.Object, ref.Body );
                    ++acted;
                }
                break;
            case FieldPhysicsType::Anchor:
                for ( const BodyRef& ref : bodies )
                {
                    Object&         object = m_Objects[ref.Object];
                    const BodyState body   = object.Bodies[ref.Body];
                    bool            newly  = false;
                    for ( const int32_t leaf : body.PartLeaf )
                    {
                        if ( object.Nodes[leaf].Anchored ||
                             !( Evaluate( command.Scalar,
                                          WorldPoint( body, object.Data->Nodes[leaf].CenterOfMass ) ) > 0.0f ) )
                            continue;
                        for ( int32_t up = leaf; up >= 0; up = object.Nodes[up].Parent )
                            object.Nodes[up].Anchored = true;
                        newly = true;
                    }
                    if ( !newly || body.Static )
                        continue;
                    // The body holds an anchored leaf now: it stays where it is, static (SpawnBody reads
                    // Anchored).
                    const glm::vec3 position = m_Physics.GetPosition( body.Handle );
                    const glm::quat rotation = m_Physics.GetRotation( body.Handle );
                    DestroyBody( ref.Object, ref.Body );
                    auto spawned = SpawnBody( ref.Object, body.Members, position, rotation, glm::vec3( 0.0f ),
                                              glm::vec3( 0.0f ), body.Broken );
                    if ( !spawned.IsSuccess() )
                        LOG_ERROR( "destructible {}: anchoring node {} failed: {}", ref.Object, body.Members[0],
                                   spawned.GetError() );
                    ++acted;
                }
                break;
        }
        return acted;
    }

    void DestructionWorld::Break( uint32_t objectIndex, uint32_t bodyIndex, const std::vector<int32_t>& released )
    {
        const BodyState old      = m_Objects[objectIndex].Bodies[bodyIndex];
        const glm::vec3 position = m_Physics.GetPosition( old.Handle );
        const glm::quat rotation = m_Physics.GetRotation( old.Handle );
        const glm::vec3 angular  = m_Physics.GetAngularVelocity( old.Handle );

        // The units left behind, grouped by connectivity (UE bCreateNewClusters).
        std::vector<std::vector<int32_t>> groups;
        groups.reserve( released.size() );
        for ( const int32_t unit : released )
            groups.push_back( { unit } );
        {
            const Object&        object = m_Objects[objectIndex];
            std::vector<int32_t> left;
            for ( const int32_t unit : UnitsOf( object, old ) )
                if ( std::find( released.begin(), released.end(), unit ) == released.end() )
                    left.push_back( unit );
            const auto connected = [&]( int32_t a, int32_t b )
            {
                for ( const int32_t la : object.Nodes[a].Leaves )
                    for ( const int32_t n : object.Nodes[la].Neighbours )
                        if ( std::find( object.Nodes[b].Leaves.begin(), object.Nodes[b].Leaves.end(), n ) !=
                             object.Nodes[b].Leaves.end() )
                            return true;
                return false;
            };
            std::vector<bool> seen( left.size(), false );
            for ( size_t start = 0; start < left.size(); ++start )
            {
                if ( seen[start] )
                    continue;
                std::vector<int32_t> group{ left[start] };
                seen[start] = true;
                for ( size_t next = 0; next < group.size(); ++next )
                    for ( size_t j = 0; j < left.size(); ++j )
                        if ( !seen[j] && connected( group[next], left[j] ) )
                        {
                            seen[j] = true;
                            group.push_back( left[j] );
                        }
                std::sort( group.begin(), group.end() );
                groups.push_back( std::move( group ) );
            }
        }

        // Each new body moves as the old body's material moved where the new body's mass is.
        std::vector<glm::vec3> linear;
        for ( const std::vector<int32_t>& group : groups )
        {
            const glm::vec3 com = position + rotation * glm::vec3( CenterOfMass( m_Objects[objectIndex], group ) );
            linear.push_back( m_Physics.GetPointVelocity( old.Handle, com ) );
        }
        DestroyBody( objectIndex, bodyIndex );

        for ( size_t g = 0; g < groups.size(); ++g )
        {
            const bool isReleased = g < released.size();
            auto       spawned = SpawnBody( objectIndex, groups[g], position, rotation, linear[g], angular, true );
            if ( !spawned.IsSuccess() )
            {
                // Every hull was cooked at Add, so only a full world refuses here; the group leaves the
                // simulation, and that is said, not hidden.
                LOG_ERROR( "destructible {}: the group of node {} could not become a body: {}", objectIndex,
                           groups[g][0], spawned.GetError() );
                continue;
            }
            if ( isReleased )
            {
                const glm::vec3 com =
                     position +
                     rotation * glm::vec3( m_Objects[objectIndex].Data->Nodes[groups[g][0]].CenterOfMass );
                m_Events.push_back(
                     DestructionEvent{ DestructionEventKind::Break, objectIndex, groups[g][0], com, linear[g] } );
            }
        }
    }
} // namespace Desert::Destruction
