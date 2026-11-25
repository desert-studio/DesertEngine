#include <Engine/Core/Serialize/WorldPartitionRules.hpp>

#include <Common/Json/DocumentStruct.hpp>

namespace Desert::Core::Rules::Detail
{
    // THE BOXES ONE RECORD OCCUPIES IN ITS OWN FRAME, each handed to @p onBox: a primitive's unit box,
    // the box around the vertices of a mesh the StaticMesh block itself carries (EditMesh, WP15c),
    // the box of each mesh asset its mesh blocks name, and - for a prefab instance - the box the prefab's
    // registry row states around its root (AL1-8a). The last two are asked of @p bounds and are skipped
    // when there is none. One rule for both askers: the partitioner flattens these to a footprint, and
    // PrefabBounds unions them into the box a prefab states, so the box a cell reads for an instance is
    // the box its instantiated body would have been given. A block that does not read is an Issue and
    // contributes no box.
    template <class OnBox>
    inline void ForEachLocalBox( const Assets::EntityData& record, const AssetBoundsSource& bounds,
                                 Common::Json::Issues& issues, OnBox&& onBox )
    {
        if ( const auto mesh = BlockOf( record, kPrimitiveComponent, issues ); mesh.has_value() )
        {
            if ( const auto primitive = mesh->Find( kPrimitiveField ) )
            {
                const std::size_t       issuesBefore = issues.size();
                Geometry::PrimitiveType shape        = {};
                primitive->ReadValue( shape, issues );
                if ( issues.size() == issuesBefore )
                {
                    if ( const auto box = Geometry::PrimitiveBounds( shape ); box.has_value() )
                        onBox( box.value() );
                }
            }
            if ( const auto box = EditMeshBox( *mesh, issues ); box.has_value() )
                onBox( box.value() );
        }

        if ( !bounds )
            return;

        for ( const std::string_view key : kMeshAssetComponents )
        {
            const auto block = BlockOf( record, key, issues );
            if ( !block.has_value() )
                continue;
            const auto mesh = MeshReferenceOf( *block, key );
            if ( !mesh.has_value() )
                continue;
            const Common::Content::AssetGuid guid = ReadGuid( mesh->Node, mesh->GuidField, issues );
            std::string                      text;
            mesh->Node.ReadInto( mesh->PathField, text, issues );
            if ( guid.IsNull() && text.empty() )
                continue;
            if ( const auto box = bounds( guid, text ); box.has_value() )
                onBox( box.value() );
        }

        // A prefab reference is a path (EntityData::PrefabPath); the registry answers it by stable key.
        if ( record.PrefabPath.has_value() && !record.PrefabPath->empty() )
        {
            if ( const auto box = bounds( Common::Content::AssetGuid{}, *record.PrefabPath ); box.has_value() )
                onBox( box.value() );
        }
    }
} // namespace Desert::Core::Rules::Detail

namespace Desert::Core::Rules
{
    [[nodiscard]] CellCoord CellOf( float worldX, float worldZ, double cellSize )
    {
        CellCoord cell;
        cell.X = static_cast<std::int32_t>( std::floor( static_cast<double>( worldX ) / cellSize ) );
        cell.Z = static_cast<std::int32_t>( std::floor( static_cast<double>( worldZ ) / cellSize ) );
        return cell;
    }

    [[nodiscard]] std::string_view GenerationIssueKindName( GenerationIssueKind kind )
    {
        switch ( kind )
        {
            case GenerationIssueKind::Planner:
                return "Planner";
            case GenerationIssueKind::InvalidReference:
                return "InvalidReference";
            case GenerationIssueKind::AlwaysLoadedReferencesSpatial:
                return "InvalidReferenceGridPlacement";
            case GenerationIssueKind::ReferenceAcrossDataLayers:
                return "InvalidReferenceDataLayers";
            case GenerationIssueKind::ReferenceIntoNoDataLayer:
                return "InvalidReferenceNoDataLayer";
            case GenerationIssueKind::ReferenceAcrossRuntimeGrids:
                return "InvalidReferenceRuntimeGrid";
            case GenerationIssueKind::StaleHLOD:
                return "StaleHLOD";
        }
        return "Unknown";
    }

    [[nodiscard]] HeightSpan UnionHeight( const HeightSpan& a, const HeightSpan& b )
    {
        return HeightSpan{ std::min( a.MinY, b.MinY ), std::max( a.MaxY, b.MaxY ) };
    }

    [[nodiscard]] double LevelCellSize( float cellSize, int level )
    {
        return std::ldexp( static_cast<double>( cellSize ), level );
    }

    [[nodiscard]] CellBounds GridSquareOf( const CellCoord& cell, double levelCellSize )
    {
        CellBounds square;
        square.MinX = static_cast<float>( static_cast<double>( cell.X ) * levelCellSize );
        square.MinZ = static_cast<float>( static_cast<double>( cell.Z ) * levelCellSize );
        square.MaxX = static_cast<float>( static_cast<double>( cell.X + 1 ) * levelCellSize );
        square.MaxZ = static_cast<float>( static_cast<double>( cell.Z + 1 ) * levelCellSize );
        return square;
    }

    [[nodiscard]] std::optional<CellCoord> SingleCellHolding( const CellBounds& footprint, double levelCellSize )
    {
        // An edge in cell units, and the float rounding it may carry (in cells too).
        constexpr double kRoundingEpsilons = 4.0 * static_cast<double>( std::numeric_limits<float>::epsilon() );
        const auto       rounding          = []( float edge, double size )
        { return kRoundingEpsilons * std::max( std::fabs( static_cast<double>( edge ) ), size ) / size; };

        // The first cell the footprint touches: a minimum a rounding short of an edge is on it.
        const auto first = [&]( float min, double size )
        { return std::floor( static_cast<double>( min ) / size + rounding( min, size ) ); };
        // The last cell the footprint touches: the one before its maximum's cell when the maximum lies on
        // an edge (or a rounding past it), never before its first cell.
        const auto last = [&]( float min, float max, double size )
        {
            return std::max( first( min, size ),
                             std::ceil( static_cast<double>( max ) / size - rounding( max, size ) ) - 1.0 );
        };

        // A coordinate whose cell index an int32 cannot hold (a corrupt 1e30, a NaN) fits no cell of this
        // level: it is not converted, because that conversion is undefined behaviour.
        for ( const float edge : { footprint.MinX, footprint.MinZ, footprint.MaxX, footprint.MaxZ } )
        {
            if ( !( std::fabs( static_cast<double>( edge ) / levelCellSize ) < 2147483647.0 ) )
                return std::nullopt;
        }

        const double firstX = first( footprint.MinX, levelCellSize );
        const double firstZ = first( footprint.MinZ, levelCellSize );
        if ( firstX != last( footprint.MinX, footprint.MaxX, levelCellSize ) ||
             firstZ != last( footprint.MinZ, footprint.MaxZ, levelCellSize ) )
            return std::nullopt;
        CellCoord cell;
        cell.X = static_cast<std::int32_t>( firstX );
        cell.Z = static_cast<std::int32_t>( firstZ );
        return cell;
    }

    [[nodiscard]] std::optional<MeshReferenceNode> MeshReferenceOf( const Common::Json::Node& block,
                                                                    std::string_view          component )
    {
        if ( component == "StaticMesh" )
        {
            auto handle = block.Find( "MeshHandle" );
            if ( !handle.has_value() )
                return std::nullopt;
            return MeshReferenceNode{ std::move( handle.value() ), "Guid", "Path" };
        }
        return MeshReferenceNode{ block, "MeshGuid", "MeshPath" };
    }

    namespace Detail
    {
        [[nodiscard]] Common::Json::Path ComponentPath( const Assets::EntityData& record, std::string_view key )
        {
            const std::string id = record.id.has_value() ? record.id->ToString() : std::string( "none" );
            return Common::Json::Path{}.Key( "Entities" ).Record( id ).Key( key );
        }

        [[nodiscard]] std::optional<Common::Json::Node> PayloadOf( const Assets::EntityData& record,
                                                                   std::string_view          key )
        {
            for ( const auto& [name, payload] : record.Components )
                if ( name == key )
                    return Common::Json::Node( payload, ComponentPath( record, key ) );
            return std::nullopt;
        }

        [[nodiscard]] std::optional<Common::Json::Node>
        BlockOf( const Assets::EntityData& record, std::string_view key, Common::Json::Issues& issues )
        {
            auto payload = PayloadOf( record, key );
            if ( !payload.has_value() )
                return std::nullopt;
            if ( payload->GetKind() != Common::Json::Kind::Object )
            {
                payload->Report( issues, "object" );
                return std::nullopt;
            }
            return payload;
        }

        [[nodiscard]] bool ReadReference( const Common::Json::Node& block, std::string_view field,
                                          Common::UUID& out, Common::Json::Issues& issues )
        {
            const auto value = block.Find( field );
            if ( !value.has_value() )
                return false;
            const std::size_t before = issues.size();
            value->ReadValue( out, issues );
            return issues.size() == before;
        }

        [[nodiscard]] glm::mat4 ComposeLocal( const Assets::EntityData& record )
        {
            const glm::vec3 translation = glm::vec3( record.Translation.value_or( glm::dvec3( 0.0 ) ) );
            const glm::vec3 rotation    = record.Rotation.value_or( glm::vec3( 0.0f ) );
            const glm::vec3 scale       = record.Scale.value_or( glm::vec3( 1.0f ) );
            return glm::translate( glm::mat4( 1.0f ), translation ) * glm::toMat4( glm::quat( rotation ) ) *
                   glm::scale( glm::mat4( 1.0f ), scale );
        }

        void AppendInstancePoints( const Assets::EntityData& record, std::vector<glm::vec3>& out,
                                   Common::Json::Issues& issues )
        {
            const auto block = BlockOf( record, kInstancePointsComponent, issues );
            if ( !block.has_value() )
                return;
            const auto field = block->Find( kInstancePointsField );
            if ( !field.has_value() )
                return;
            if ( field->GetKind() != Common::Json::Kind::Array )
            {
                field->Report( issues, "array of matrices" );
                return;
            }

            field->ForEachElement(
                 [&]( std::size_t, const Common::Json::Node& matrix )
                 {
                     std::size_t count = 0;
                     if ( matrix.GetKind() == Common::Json::Kind::Array )
                         matrix.ForEachElement( [&]( std::size_t, const Common::Json::Node& ) { ++count; } );
                     if ( count != 16 )
                     {
                         matrix.Report( issues, "array of 16 numbers" );
                         return;
                     }
                     const std::size_t before = issues.size();
                     float             x      = 0.0f;
                     float             y      = 0.0f;
                     float             z      = 0.0f;
                     matrix.ForEachElement(
                          [&]( std::size_t i, const Common::Json::Node& element )
                          {
                              if ( i == 12 )
                                  element.ReadValue( x, issues );
                              else if ( i == 13 )
                                  element.ReadValue( y, issues );
                              else if ( i == 14 )
                                  element.ReadValue( z, issues );
                          } );
                     if ( issues.size() == before )
                         out.emplace_back( x, y, z );
                 } );
        }

        [[nodiscard]] bool ReadScalar( const Common::Json::Node& block, std::string_view field, double& out,
                                       Common::Json::Issues& issues )
        {
            const auto value = block.Find( field );
            if ( !value.has_value() )
                return false;
            if ( const auto flag = value->AsBool() )
            {
                out = flag.GetValue() ? 1.0 : 0.0;
                return true;
            }
            if ( const auto number = value->AsNumber() )
            {
                out = number.GetValue();
                return true;
            }
            value->Report( issues, "bool or number" );
            return false;
        }

        [[nodiscard]] AlwaysLoadedReason GlobalReasonOf( const Assets::EntityData& record,
                                                         Common::Json::Issues&     issues )
        {
            if ( PayloadOf( record, kAlwaysLoadedComponent ).has_value() )
                return AlwaysLoadedReason::Author;

            for ( const ComponentLoadingRow& row : kComponentLoading )
            {
                if ( row.Loading == ComponentLoading::Spatial )
                    continue;
                if ( !PayloadOf( record, row.ComponentKey ).has_value() )
                    continue;
                if ( row.Loading == ComponentLoading::Global )
                    return AlwaysLoadedReason::Component;

                bool global = row.AbsentIsGlobal;
                if ( const auto block = BlockOf( record, row.ComponentKey, issues ); block.has_value() )
                {
                    double value = 0.0;
                    if ( ReadScalar( *block, row.Field, value, issues ) )
                        global = value != row.SpatialWhen;
                }
                if ( global )
                    return AlwaysLoadedReason::Component;
            }
            return AlwaysLoadedReason::None;
        }

        void AppendBoxCorners( const glm::mat4& world, const glm::vec3& half, std::vector<glm::vec3>& out )
        {
            for ( int corner = 0; corner < 8; ++corner )
            {
                const glm::vec4 local( ( ( corner & 1 ) != 0 ) ? half.x : -half.x,
                                       ( ( corner & 2 ) != 0 ) ? half.y : -half.y,
                                       ( ( corner & 4 ) != 0 ) ? half.z : -half.z, 1.0f );
                const glm::vec4 placed = world * local;
                out.emplace_back( placed.x, placed.y, placed.z );
            }
        }

        void AppendBoundsCorners( const glm::mat4& world, const Common::Math::AABB& box,
                                  std::vector<glm::vec3>& out )
        {
            const glm::vec3 centre = ( box.Min + box.Max ) * 0.5f;
            const glm::vec3 half   = ( box.Max - box.Min ) * 0.5f;
            AppendBoxCorners( world * glm::translate( glm::mat4( 1.0f ), centre ), half, out );
        }

        [[nodiscard]] Common::Content::AssetGuid ReadGuid( const Common::Json::Node& block, std::string_view field,
                                                           Common::Json::Issues& issues )
        {
            std::string text;
            block.ReadInto( field, text, issues );
            if ( text.empty() )
                return {};
            const auto guid = Common::Content::AssetGuidFromText( text );
            if ( !guid )
            {
                if ( const auto value = block.Find( field ) )
                    value->Report( issues, "GUID text" );
                return {};
            }
            return guid.GetValue();
        }

        [[nodiscard]] std::optional<Common::Math::AABB> EditMeshBox( const Common::Json::Node& block,
                                                                     Common::Json::Issues&     issues )
        {
            const auto editMesh = block.Find( kEditMeshField );
            if ( !editMesh.has_value() || !editMesh->ExpectKind( Common::Json::Kind::Object, issues ) )
                return std::nullopt;
            const auto field = editMesh->Find( kEditMeshPositionsField );
            if ( !field.has_value() )
            {
                editMesh->Report( issues, "an EditMesh with a Positions array" );
                return std::nullopt;
            }
            const std::size_t  before = issues.size();
            std::vector<float> positions;
            field->ReadValue( positions, issues );
            if ( issues.size() != before )
                return std::nullopt;
            if ( positions.empty() || positions.size() % 3 != 0 )
            {
                field->Report( issues, "a non-empty array of xyz triples" );
                return std::nullopt;
            }
            Common::Math::AABB box{ glm::vec3( positions[0], positions[1], positions[2] ),
                                    glm::vec3( positions[0], positions[1], positions[2] ) };
            for ( std::size_t vertex = 3; vertex < positions.size(); vertex += 3 )
            {
                const glm::vec3 point( positions[vertex], positions[vertex + 1], positions[vertex + 2] );
                box.Min = glm::min( box.Min, point );
                box.Max = glm::max( box.Max, point );
            }
            return box;
        }

    } // namespace Detail

    namespace
    {
        // The registered DescriptorHooks (UE: the loaded actor classes' descriptor types). Filled at module
        // startup, read by DescribeEntity and the planner.
        std::vector<DescriptorHook>& HookList()
        {
            static std::vector<DescriptorHook> hooks;
            return hooks;
        }

        const DescriptorHook* FindHook( std::string_view component )
        {
            for ( const DescriptorHook& hook : HookList() )
                if ( hook.Component == component )
                    return &hook;
            return nullptr;
        }

        const DescriptorFacts* FactsOf( const EntityDescriptor& descriptor, std::string_view component )
        {
            for ( const DescriptorFacts& facts : descriptor.Facts )
                if ( facts.Component == component )
                    return &facts;
            return nullptr;
        }
    } // namespace

    void RegisterDescriptorHook( const DescriptorHook& hook )
    {
        if ( hook.Component.empty() || hook.Describe == nullptr || FindHook( hook.Component ) != nullptr )
            return;
        HookList().push_back( hook );
    }

    std::span<const DescriptorHook> DescriptorHooks()
    {
        return HookList();
    }

    [[nodiscard]] std::vector<UnregisteredReference>
    FindUnregisteredEntityReferences( std::span<const Assets::EntityData> records )
    {
        std::unordered_map<Common::UUID, std::size_t> byId;
        for ( std::size_t record = 0; record < records.size(); ++record )
        {
            if ( const auto& id = records[record].id; id.has_value() && !id->IsNull() )
                byId.emplace( *id, record );
        }

        std::vector<UnregisteredReference> found;
        for ( std::size_t record = 0; record < records.size(); ++record )
        {
            for ( const auto& [componentKey, payload] : records[record].Components )
            {
                // A census by SHAPE, not a read of a field: a value that is not an id is simply not a
                // reference, so nothing here is an Issue.
                Common::Json::Root( payload ).ForEachMember(
                     [&]( std::string_view field, const Common::Json::Node& value )
                     {
                         if ( value.GetKind() != Common::Json::Kind::String )
                             return;
                         const auto id = value.AsUuid();
                         if ( !id || id.GetValue().IsNull() )
                             return;
                         const Common::UUID named = id.GetValue();
                         if ( byId.find( named ) == byId.end() )
                             return;

                         bool registered = false;
                         for ( const EntityReferenceRow& row : kEntityReferences )
                         {
                             if ( componentKey == row.ComponentKey && field == row.Field )
                             {
                                 registered = true;
                                 break;
                             }
                         }
                         if ( !registered )
                             found.push_back(
                                  UnregisteredReference{ record, componentKey, std::string( field ), named } );
                     } );
            }
        }
        return found;
    }

    namespace Detail
    {
        [[nodiscard]] std::optional<std::uint64_t> IdBits( const std::optional<Common::UUID>& id )
        {
            if ( !id.has_value() || id->IsNull() )
                return std::nullopt;
            return static_cast<std::uint64_t>( *id );
        }

    } // namespace Detail
    [[nodiscard]] EntityDescriptor DescribeEntity( const Assets::EntityData& record )
    {
        EntityDescriptor      out;
        Common::Json::Issues& issues = out.Issues;
        out.Id                       = Detail::IdBits( record.id );
        out.Parent                   = Detail::IdBits( record.parent );
        out.Tag                      = record.Tag.value_or( "" );
        for ( const auto& [name, payload] : record.Components )
            out.Components.push_back( name );
        out.Translation      = record.Translation;
        out.Rotation         = record.Rotation;
        out.Scale            = record.Scale;
        out.PrefabPath       = record.PrefabPath;
        out.MissingTransform = Assets::MissingInstanceTransform( record );

        // The record's own boxes, and the mesh assets whose boxes the bounds source states.
        Detail::ForEachLocalBox( record, AssetBoundsSource{}, issues,
                                 [&]( const Common::Math::AABB& box ) { out.Boxes.push_back( box ); } );
        for ( const std::string_view key : kMeshAssetComponents )
        {
            const auto block = Detail::BlockOf( record, key, issues );
            if ( !block.has_value() )
                continue;
            const auto mesh = MeshReferenceOf( *block, key );
            if ( !mesh.has_value() )
                continue;
            DescriptorMesh                   named;
            const Common::Content::AssetGuid guid = Detail::ReadGuid( mesh->Node, mesh->GuidField, issues );
            mesh->Node.ReadInto( mesh->PathField, named.Path, issues );
            if ( guid.IsNull() && named.Path.empty() )
                continue;
            if ( !guid.IsNull() )
                named.Guid = Common::Content::AssetGuidToText( guid );
            out.Meshes.push_back( std::move( named ) );
        }
        if ( record.PrefabPath.has_value() && !record.PrefabPath->empty() )
            out.Meshes.push_back( DescriptorMesh{ std::string(), *record.PrefabPath } );
        Detail::AppendInstancePoints( record, out.InstancePoints, issues );

        out.Reason = Detail::GlobalReasonOf( record, issues );

        for ( const EntityReferenceRow& row : kEntityReferences )
        {
            const auto block = Detail::BlockOf( record, row.ComponentKey, issues );
            if ( !block.has_value() )
                continue;
            Common::UUID target;
            if ( !Detail::ReadReference( *block, row.Field, target, issues ) || target.IsNull() )
                continue;
            out.References.push_back( DescriptorReference{ std::string( row.ComponentKey ),
                                                           std::string( row.Field ),
                                                           static_cast<std::uint64_t>( target ), row.Kind } );
        }

        // The Entity properties its script slots override - kept with the script and the property, whose
        // declaration answers at planning time what each one is (ScriptReferenceSource).
        if ( const auto script = Detail::BlockOf( record, kScriptComponent, issues ); script.has_value() )
        {
            const auto slots = script->Find( "Scripts" );
            if ( slots.has_value() && slots->ExpectKind( Common::Json::Kind::Array, issues ) )
            {
                slots->ForEachElement(
                     [&]( std::size_t, const Common::Json::Node& slot )
                     {
                         if ( !slot.ExpectKind( Common::Json::Kind::Object, issues ) )
                             return;
                         std::string key;
                         slot.ReadInto( "ScriptKey", key, issues );
                         const auto props = slot.Find( "Props" );
                         if ( key.empty() || !props.has_value() ||
                              !props->ExpectKind( Common::Json::Kind::Array, issues ) )
                             return;
                         props->ForEachElement(
                              [&]( std::size_t, const Common::Json::Node& prop )
                              {
                                  if ( !prop.ExpectKind( Common::Json::Kind::Object, issues ) )
                                      return;
                                  std::string name;
                                  int         type    = -1;
                                  int         element = -1;
                                  prop.ReadInto( "Name", name, issues );
                                  prop.ReadInto( "Type", type, issues );
                                  prop.ReadInto( "ElementType", element, issues );
                                  // Ref is the UUID's bits as the Script serializer's mirror writes a uint64 (a
                                  // JSON integer, int64: an id at or above 2^63 is spelt negative), so it is read
                                  // as int64 and taken back bit for bit - a uint64 read refuses half of all ids.
                                  const auto add = [&]( const Common::Json::Node& value )
                                  {
                                      std::int64_t bits = 0;
                                      value.ReadInto( "Ref", bits, issues );
                                      const auto target = static_cast<std::uint64_t>( bits );
                                      if ( target != 0 )
                                          out.ScriptReferences.push_back(
                                               DescriptorScriptReference{ key, name, target } );
                                  };
                                  constexpr int kEntity = static_cast<int>( Scripting::PropertyType::Entity );
                                  constexpr int kArray  = static_cast<int>( Scripting::PropertyType::Array );
                                  if ( type == kEntity )
                                      add( prop );
                                  else if ( type == kArray && element == kEntity )
                                  {
                                      const auto items = prop.Find( "Items" );
                                      if ( items.has_value() &&
                                           items->ExpectKind( Common::Json::Kind::Array, issues ) )
                                          items->ForEachElement(
                                               [&]( std::size_t, const Common::Json::Node& item )
                                               {
                                                   if ( item.ExpectKind( Common::Json::Kind::Object, issues ) )
                                                       add( item );
                                               } );
                                  }
                              } );
                     } );
            }
        }

        // The names it holds for kEntityReferencesByName rows, and the names it carries as a row's target.
        for ( const NamedEntityReferenceRow& row : kEntityReferencesByName )
        {
            if ( const auto block = Detail::BlockOf( record, row.ComponentKey, issues ); block.has_value() )
            {
                std::string name;
                block->ReadInto( row.Field, name, issues );
                if ( !name.empty() )
                    out.NamedReferences.push_back( DescriptorNamedReference{
                         std::string( row.ComponentKey ), std::string( row.Field ), std::move( name ) } );
            }
            const bool carried =
                 std::any_of( out.Names.begin(), out.Names.end(), [&row]( const DescriptorName& held )
                              { return held.Component == row.TargetComponentKey; } );
            if ( carried )
                continue;
            if ( const auto target = Detail::BlockOf( record, row.TargetComponentKey, issues );
                 target.has_value() )
            {
                std::string name;
                target->ReadInto( "Name", name, issues );
                if ( !name.empty() )
                    out.Names.push_back(
                         DescriptorName{ std::string( row.TargetComponentKey ), std::move( name ) } );
            }
        }

        if ( const auto grid = Detail::BlockOf( record, kRuntimeGridComponent, issues ); grid.has_value() )
        {
            std::string name;
            grid->ReadInto( "Grid", name, issues );
            out.RuntimeGrid = std::move( name );
        }
        if ( const auto source = Detail::BlockOf( record, kStreamingSourceComponent, issues ); source.has_value() )
        {
            source->ReadInto( "TargetGrids", out.TargetGrids, issues );
            source->ReadInto( "TargetHLODLayers", out.TargetHLODLayers, issues );
        }
        if ( const auto layers = Detail::BlockOf( record, kDataLayersComponent, issues ); layers.has_value() )
            layers->ReadInto( "Layers", out.DataLayers, issues );
        if ( const auto volume = Detail::BlockOf( record, kLocationVolumeComponent, issues ); volume.has_value() )
        {
            DescriptorLocationVolume box;
            volume->ReadInto( "Extent", box.Extent, issues );
            volume->ReadInto( "AutoLoad", box.AutoLoad, issues );
            out.LocationVolume = box;
        }
        if ( const auto hlod = Detail::BlockOf( record, kHLODLayerComponent, issues ); hlod.has_value() )
        {
            std::string name;
            hlod->ReadInto( "Layer", name, issues );
            out.HLODLayer = std::move( name );
        }

        // The components that describe their own record (DescriptorHook), in registration order. A record that
        // carries the component with a payload that is not a block is handed over too (block null): the hook
        // decides whether that is "no place" (a landscape tile) or nothing at all.
        for ( const DescriptorHook& hook : DescriptorHooks() )
        {
            const auto block = Detail::BlockOf( record, hook.Component, issues );
            if ( !block.has_value() && !Detail::PayloadOf( record, hook.Component ).has_value() )
                continue;
            DescriptorFacts facts;
            facts.Component = std::string( hook.Component );
            if ( hook.Describe( block.has_value() ? &*block : nullptr, facts, issues ) )
                out.Facts.push_back( std::move( facts ) );
        }
        return out;
    }

    [[nodiscard]] std::vector<EntityDescriptor> DescribeEntities( std::span<const Assets::EntityData> records )
    {
        std::vector<EntityDescriptor> out;
        out.reserve( records.size() );
        for ( const Assets::EntityData& record : records )
            out.push_back( DescribeEntity( record ) );
        return out;
    }

    [[nodiscard]] Containment ContainmentOf( ReferenceOrigin origin )
    {
        if ( origin == ReferenceOrigin::Script )
            return Containment::ScriptReference;
        if ( origin == ReferenceOrigin::Name )
            return Containment::NamedReference;
        return Containment::SocketAttachment; // the register's one Containment row
    }

    [[nodiscard]] std::vector<ResolvedReference>
    ResolveEntityReferences( std::span<const EntityDescriptor> records, const ScriptReferenceSource& scripts,
                             Common::Json::Issues& issues )
    {
        std::unordered_map<Common::UUID, std::size_t> byId;
        std::map<std::pair<std::string, std::string>, std::vector<std::size_t>>
             carriers; // (block, name) -> records
        for ( std::size_t record = 0; record < records.size(); ++record )
        {
            if ( records[record].Id.has_value() )
                byId.emplace( Common::UUID( *records[record].Id ), record );
            for ( const DescriptorName& name : records[record].Names )
                carriers[{ name.Component, name.Name }].push_back( record );
        }
        const auto where = [&records]( std::size_t record )
        {
            return records[record].Id.has_value() ? std::format( "Entities[id={}]", *records[record].Id )
                                                  : std::format( "Entities[{}]", record );
        };
        const auto recordId = [&records]( std::size_t record )
        { return records[record].Id.has_value() ? Common::UUID( *records[record].Id ) : Common::UUID::Null(); };
        const auto target = [&]( ResolvedReference& reference, std::uint64_t id )
        {
            reference.TargetId = Common::UUID( id );
            if ( const auto found = byId.find( *reference.TargetId ); found != byId.end() )
            {
                reference.To             = found->second;
                reference.TargetRecordId = *reference.TargetId;
            }
        };

        std::vector<ResolvedReference> out;
        std::size_t                    undeclared = 0;
        for ( std::size_t record = 0; record < records.size(); ++record )
        {
            const EntityDescriptor& data = records[record];
            for ( const DescriptorReference& reference : data.References )
            {
                ResolvedReference resolved{ .From      = record,
                                            .Kind      = reference.Kind,
                                            .Origin    = ReferenceOrigin::Register,
                                            .Component = reference.Component,
                                            .Field     = reference.Field };
                target( resolved, reference.Target );
                out.push_back( std::move( resolved ) );
            }
            for ( const DescriptorScriptReference& reference : data.ScriptReferences )
            {
                ResolvedReference resolved{ .From      = record,
                                            .Kind      = ReferenceKind::Observation,
                                            .Origin    = ReferenceOrigin::Script,
                                            .Component = std::string( kScriptComponent ),
                                            .Field =
                                                 std::format( "{}:{}", reference.Script, reference.Property ) };
                if ( !scripts )
                    ++undeclared;
                else if ( const auto kind = scripts( reference.Script, reference.Property ); kind.has_value() )
                    resolved.Kind = *kind;
                else
                    issues.push_back( Common::Json::Issue{
                         std::format( "{}.{}", where( record ), kScriptComponent ),
                         std::format( "an Entity property '{}' that '{}' declares", reference.Property,
                                      reference.Script ),
                         "none: the script does not declare it as an Entity or does not load, so the reference "
                         "observes" } );
                target( resolved, reference.Target );
                out.push_back( std::move( resolved ) );
            }
            for ( const DescriptorNamedReference& reference : data.NamedReferences )
            {
                const auto row = std::find_if(
                     std::begin( kEntityReferencesByName ), std::end( kEntityReferencesByName ),
                     [&reference]( const NamedEntityReferenceRow& named )
                     { return named.ComponentKey == reference.Component && named.Field == reference.Field; } );
                if ( row == std::end( kEntityReferencesByName ) )
                {
                    issues.push_back( Common::Json::Issue{
                         std::format( "{}.{}.{}", where( record ), reference.Component, reference.Field ),
                         "a reference by name kEntityReferencesByName has a row for",
                         "none: the descriptor is of another build; refresh the descriptor index" } );
                    continue;
                }
                ResolvedReference resolved{ .From      = record,
                                            .Kind      = row->Kind,
                                            .Origin    = ReferenceOrigin::Name,
                                            .Component = reference.Component,
                                            .Field     = reference.Field };
                const auto carrying = carriers.find( { std::string( row->TargetComponentKey ), reference.Name } );
                if ( carrying == carriers.end() )
                {
                    if ( row->Kind == ReferenceKind::Containment )
                        issues.push_back( Common::Json::Issue{
                             std::format( "{}.{}.{}", where( record ), reference.Component, reference.Field ),
                             std::format( "an entity whose {} is named '{}'", row->TargetComponentKey,
                                          reference.Name ),
                             "none: the containment holds nothing" } );
                    out.push_back( std::move( resolved ) );
                    continue;
                }
                for ( const std::size_t carrier : carrying->second )
                {
                    if ( carrier == record )
                        continue;
                    ResolvedReference matched = resolved;
                    matched.To                = carrier;
                    matched.TargetRecordId    = recordId( carrier );
                    out.push_back( std::move( matched ) );
                }
            }
        }
        if ( undeclared > 0 )
            issues.push_back( Common::Json::Issue{
                 "Entities", "the scripts' declarations (a ScriptReferenceSource)",
                 std::format( "none given: {} script entity reference(s) planned as observation", undeclared ) } );
        return out;
    }

    namespace Detail
    {
        [[nodiscard]] std::optional<Common::UUID> ParentOf( const Assets::EntityData& record )
        {
            if ( record.parent.has_value() && !record.parent->IsNull() )
                return record.parent;
            return std::nullopt;
        }

        [[nodiscard]] std::optional<Common::UUID> ParentOf( const EntityDescriptor& descriptor )
        {
            if ( descriptor.Parent.has_value() )
                return Common::UUID( *descriptor.Parent );
            return std::nullopt;
        }

        [[nodiscard]] glm::mat4 ComposeLocal( const EntityDescriptor& descriptor )
        {
            const glm::vec3 translation = descriptor.Translation.value_or( glm::vec3( 0.0f ) );
            const glm::vec3 rotation    = descriptor.Rotation.value_or( glm::vec3( 0.0f ) );
            const glm::vec3 scale       = descriptor.Scale.value_or( glm::vec3( 1.0f ) );
            return glm::translate( glm::mat4( 1.0f ), translation ) * glm::toMat4( glm::quat( rotation ) ) *
                   glm::scale( glm::mat4( 1.0f ), scale );
        }

        bool AppendFootprint( const EntityDescriptor& descriptor, const glm::mat4& world,
                              const AssetBoundsSource& bounds, std::vector<glm::vec3>& out )
        {
            out.emplace_back( world[3].x, world[3].y, world[3].z );
            const std::size_t before = out.size();
            for ( const Common::Math::AABB& box : descriptor.Boxes )
                AppendBoundsCorners( world, box, out );
            if ( bounds )
                for ( const DescriptorMesh& mesh : descriptor.Meshes )
                {
                    Common::Content::AssetGuid guid;
                    if ( !mesh.Guid.empty() )
                        if ( const auto parsed = Common::Content::AssetGuidFromText( mesh.Guid ) )
                            guid = parsed.GetValue();
                    if ( const auto box = bounds( guid, mesh.Path ); box.has_value() )
                        AppendBoundsCorners( world, box.value(), out );
                }
            out.insert( out.end(), descriptor.InstancePoints.begin(), descriptor.InstancePoints.end() );
            return out.size() > before;
        }

        HookedFootprint HookFootprintOf( std::size_t record, std::span<const EntityDescriptor> records,
                                         const std::vector<glm::mat4>&                        world,
                                         const std::unordered_map<Common::UUID, std::size_t>& byId )
        {
            for ( const DescriptorFacts& own : records[record].Facts )
            {
                const DescriptorHook* hook = FindHook( own.Component );
                if ( hook == nullptr || hook->Footprint == nullptr )
                    continue;
                const DescriptorFacts* frame      = nullptr;
                glm::mat4              frameWorld = world[record];
                if ( !hook->FrameComponent.empty() )
                {
                    frameWorld = glm::mat4( 1.0f );
                    if ( own.Frame != 0 )
                        if ( const auto found = byId.find( Common::UUID( own.Frame ) ); found != byId.end() )
                        {
                            frame      = FactsOf( records[found->second], hook->FrameComponent );
                            frameWorld = world[found->second];
                        }
                }
                return HookedFootprint{ true, hook->Footprint( own, frame, frameWorld ) };
            }
            return {};
        }

    } // namespace Detail
    std::string ReportGenerationIssues( std::span<const GenerationIssue> issues, std::string_view world )
    {
        std::string text;
        for ( const GenerationIssue& issue : issues )
        {
            if ( issue.Kind == GenerationIssueKind::Planner )
                continue;
            const std::string line = std::format( "[WorldPartition] '{}': {}: {}", world,
                                                  GenerationIssueKindName( issue.Kind ), issue.Text );
            LOG_WARN( "{}", line );
            text += ( text.empty() ? "" : "\n" ) + line;
        }
        return text;
    }

    std::string ReportPlanIssues( const WorldPartitionPlan& plan, std::string_view world )
    {
        std::string text =
             Common::Json::ReportIssues( plan.Issues, std::format( "the world partition plan of '{}'", world ) );
        const std::string found = ReportGenerationIssues( plan.Generation, world );
        if ( !text.empty() && !found.empty() )
            text += "\n";
        return text + found;
    }

    [[nodiscard]] std::vector<GenerationIssue> PlanIssues( const WorldPartitionPlan&         plan,
                                                           std::span<const EntityDescriptor> records )
    {
        std::vector<GenerationIssue> issues;
        issues.reserve( plan.Issues.size() + plan.Generation.size() );
        constexpr std::string_view kEntities = "Entities[";
        for ( const Common::Json::Issue& issue : plan.Issues )
        {
            GenerationIssue named;
            named.Text = Common::Json::Describe( issue );
            if ( issue.Path.starts_with( kEntities ) )
            {
                std::string_view rest = std::string_view( issue.Path ).substr( kEntities.size() );
                const bool       byId = rest.starts_with( "id=" );
                rest                  = byId ? rest.substr( 3 ) : rest;
                std::uint64_t value   = 0;
                std::size_t   digits  = 0;
                while ( digits < rest.size() && rest[digits] >= '0' && rest[digits] <= '9' )
                {
                    value = value * 10 + static_cast<std::uint64_t>( rest[digits] - '0' );
                    ++digits;
                }
                if ( digits > 0 && digits < rest.size() && rest[digits] == ']' )
                {
                    if ( !byId && value < records.size() )
                        named.Record = static_cast<std::size_t>( value );
                    for ( std::size_t record = 0; byId && record < records.size(); ++record )
                        if ( records[record].Id == value )
                        {
                            named.Record = record;
                            break;
                        }
                }
            }
            issues.push_back( std::move( named ) );
        }
        issues.insert( issues.end(), plan.Generation.begin(), plan.Generation.end() );
        return issues;
    }

    [[nodiscard]] std::optional<std::size_t> GridIndexOf( const WorldPartitionSerialized&   settings,
                                                          const std::optional<std::string>& wanted )
    {
        const std::vector<RuntimePartitionSerialized>& partitions = settings.RuntimePartitions;
        if ( partitions.empty() )
            return std::nullopt;
        if ( !wanted.has_value() )
            return std::size_t{ 0 };
        const auto found =
             std::find_if( partitions.begin(), partitions.end(), [&]( const RuntimePartitionSerialized& partition )
                           { return partition.Name == *wanted; } );
        if ( found == partitions.end() )
            return std::nullopt;
        return static_cast<std::size_t>( found - partitions.begin() );
    }

    [[nodiscard]] Common::Json::Issues CheckDataLayers( const WorldPartitionSerialized& settings )
    {
        Common::Json::Issues issues;
        if ( !settings.DataLayers.has_value() )
            return issues;
        const std::vector<DataLayerSerialized>& layers = *settings.DataLayers;
        for ( std::size_t layerIndex = 0; layerIndex < layers.size(); ++layerIndex )
        {
            const DataLayerSerialized& layer = layers[layerIndex];
            const bool                 twice =
                 std::any_of( layers.begin(), layers.begin() + static_cast<std::ptrdiff_t>( layerIndex ),
                              [&]( const DataLayerSerialized& other ) { return other.Name == layer.Name; } );
            if ( layer.Name.empty() || twice )
                issues.push_back( { std::format( "WorldPartition.DataLayers[{}].Name", layerIndex ),
                                    "a name no other data layer of the world states", layer.Name } );
            // WP v3: a runtime layer starts in the state it states; an editor layer has none to start in.
            if ( layer.Type == DataLayerType::Runtime && !layer.InitialRuntimeState.has_value() )
                issues.push_back(
                     { std::format( "WorldPartition.DataLayers[{}].InitialRuntimeState", layerIndex ),
                       "the state a Runtime data layer starts Play in (Unloaded, Loaded or Activated)",
                       "nothing" } );
            if ( layer.Type == DataLayerType::Editor && layer.InitialRuntimeState.has_value() )
                issues.push_back( { std::format( "WorldPartition.DataLayers[{}].InitialRuntimeState", layerIndex ),
                                    "no runtime state on an Editor data layer (it does not stream)",
                                    "a runtime state" } );
            // WP v4 (UE UDataLayerInstance::CanBeChildOf): a parent is ANOTHER layer of this world, and a Runtime
            // layer's parent is a Runtime layer - its state is clamped by the parent's, which an Editor layer has
            // not.
            if ( layer.Parent.has_value() )
            {
                const auto parent =
                     std::find_if( layers.begin(), layers.end(), [&]( const DataLayerSerialized& other )
                                   { return other.Name == *layer.Parent; } );
                if ( parent == layers.end() || *layer.Parent == layer.Name )
                    issues.push_back( { std::format( "WorldPartition.DataLayers[{}].Parent", layerIndex ),
                                        "the name of another data layer of the world", *layer.Parent } );
                else if ( layer.Type == DataLayerType::Runtime && parent->Type != DataLayerType::Runtime )
                    issues.push_back( { std::format( "WorldPartition.DataLayers[{}].Parent", layerIndex ),
                                        "a Runtime data layer (a Runtime layer is gated by its parent's state)",
                                        std::format( "the Editor data layer '{}'", *layer.Parent ) } );
            }
        }
        // A chain of parents ends at a top-level layer: a cycle would make "every ancestor" endless. Walked at
        // most one step per layer, so a chain still going after that has closed a loop.
        const auto parentOf = [&layers]( const std::size_t index ) -> std::optional<std::size_t>
        {
            if ( !layers[index].Parent.has_value() )
                return std::nullopt;
            const auto parent = std::find_if( layers.begin(), layers.end(), [&]( const DataLayerSerialized& other )
                                              { return other.Name == *layers[index].Parent; } );
            if ( parent == layers.end() )
                return std::nullopt;
            return static_cast<std::size_t>( parent - layers.begin() );
        };
        for ( std::size_t layerIndex = 0; layerIndex < layers.size(); ++layerIndex )
        {
            std::optional<std::size_t> at    = parentOf( layerIndex );
            std::size_t                steps = 0;
            while ( at.has_value() && steps < layers.size() )
            {
                at = parentOf( *at );
                ++steps;
            }
            if ( at.has_value() )
                issues.push_back( { std::format( "WorldPartition.DataLayers[{}].Parent", layerIndex ),
                                    "a chain of parents that ends at a top-level data layer", "a cycle" } );
        }
        return issues;
    }

    [[nodiscard]] std::vector<std::size_t> RuntimeDataLayersOf( const WorldPartitionSerialized& settings,
                                                                std::span<const std::size_t>    layers )
    {
        std::vector<std::size_t> runtime;
        if ( !settings.DataLayers.has_value() )
            return runtime;
        for ( const std::size_t layer : layers )
            if ( layer < settings.DataLayers->size() &&
                 ( *settings.DataLayers )[layer].Type == DataLayerType::Runtime )
                runtime.push_back( layer );
        return runtime;
    }

    [[nodiscard]] std::string DataLayersCellSuffix( const WorldPartitionSerialized& settings,
                                                    std::span<const std::size_t>    runtimeLayers )
    {
        if ( runtimeLayers.empty() || !settings.DataLayers.has_value() )
            return {};
        std::vector<std::string_view> names;
        names.reserve( runtimeLayers.size() );
        for ( const std::size_t layer : runtimeLayers )
            names.emplace_back( settings.DataLayers->at( layer ).Name );
        std::sort( names.begin(), names.end() );
        std::uint32_t hash = 2166136261u;
        const auto    mix  = [&hash]( unsigned char byte )
        {
            hash ^= byte;
            hash *= 16777619u;
        };
        for ( const std::string_view name : names )
        {
            for ( const char c : name )
                mix( static_cast<unsigned char>( c ) );
            mix( 0 );
        }
        return std::format( "_DL{:08X}", hash );
    }

    [[nodiscard]] std::string CellStem( const WorldPartitionSerialized& settings, const PlannedCell& cell )
    {
        return std::format( "G{}_L{}_{}_{}{}", cell.Grid, cell.Level, cell.Cell.X, cell.Cell.Z,
                            DataLayersCellSuffix( settings, cell.DataLayers ) );
    }

    [[nodiscard]] std::optional<std::size_t> HLODLayerIndexOf( const WorldPartitionSerialized& settings,
                                                               std::string_view                name )
    {
        if ( !settings.HLODLayers.has_value() )
            return std::nullopt;
        const std::vector<HLODLayerSerialized>& layers = *settings.HLODLayers;
        const auto                              found  = std::find_if( layers.begin(), layers.end(),
                                                                       [&]( const HLODLayerSerialized& layer ) { return layer.Name == name; } );
        if ( found == layers.end() )
            return std::nullopt;
        return static_cast<std::size_t>( found - layers.begin() );
    }

    [[nodiscard]] std::optional<std::size_t> DataLayerIndexOf( const WorldPartitionSerialized& settings,
                                                               std::string_view                name )
    {
        if ( !settings.DataLayers.has_value() )
            return std::nullopt;
        const std::vector<DataLayerSerialized>& layers = *settings.DataLayers;
        const auto                              found  = std::find_if( layers.begin(), layers.end(),
                                                                       [&]( const DataLayerSerialized& layer ) { return layer.Name == name; } );
        if ( found == layers.end() )
            return std::nullopt;
        return static_cast<std::size_t>( found - layers.begin() );
    }

    [[nodiscard]] std::optional<std::size_t> DataLayerParentIndex( const WorldPartitionSerialized& settings,
                                                                   const std::size_t               layer )
    {
        if ( !settings.DataLayers.has_value() || layer >= settings.DataLayers->size() )
            return std::nullopt;
        const std::optional<std::string>& parent = ( *settings.DataLayers )[layer].Parent;
        if ( !parent.has_value() )
            return std::nullopt;
        return DataLayerIndexOf( settings, *parent );
    }

    [[nodiscard]] bool DataLayerIsWithin( const WorldPartitionSerialized& settings, const std::size_t layer,
                                          const std::size_t ancestor )
    {
        const std::size_t          count = settings.DataLayers.has_value() ? settings.DataLayers->size() : 0;
        std::optional<std::size_t> at    = layer;
        for ( std::size_t steps = 0; at.has_value() && steps <= count; ++steps )
        {
            if ( *at == ancestor )
                return true;
            at = DataLayerParentIndex( settings, *at );
        }
        return false;
    }

    [[nodiscard]] std::vector<bool> EffectiveDataLayerFlags( const WorldPartitionSerialized& settings,
                                                             const std::vector<bool>&        own )
    {
        std::vector<bool> effective = own;
        for ( std::size_t layer = 0; layer < effective.size(); ++layer )
        {
            std::optional<std::size_t> at = DataLayerParentIndex( settings, layer );
            for ( std::size_t steps = 0; effective[layer] && at.has_value() && steps < own.size(); ++steps )
            {
                effective[layer] = *at < own.size() && own[*at];
                at               = DataLayerParentIndex( settings, *at );
            }
        }
        return effective;
    }

    [[nodiscard]] std::array<float, 3> SeededDataLayerColor( std::string_view name )
    {
        std::uint32_t hash = 2166136261u;
        for ( const char character : name )
        {
            hash ^= static_cast<std::uint8_t>( character );
            hash *= 16777619u;
        }
        const float          hue = static_cast<float>( hash % 360u ) / 60.0f; // the sector of the hue circle, 0..6
        const float          value     = 0.9f;
        const float          chroma    = value * 0.65f;
        const float          secondary = chroma * ( 1.0f - std::abs( std::fmod( hue, 2.0f ) - 1.0f ) );
        const float          base      = value - chroma;
        std::array<float, 3> rgb{};
        switch ( static_cast<int>( hue ) )
        {
            case 0:
                rgb = { chroma, secondary, 0.0f };
                break;
            case 1:
                rgb = { secondary, chroma, 0.0f };
                break;
            case 2:
                rgb = { 0.0f, chroma, secondary };
                break;
            case 3:
                rgb = { 0.0f, secondary, chroma };
                break;
            case 4:
                rgb = { secondary, 0.0f, chroma };
                break;
            default:
                rgb = { chroma, 0.0f, secondary };
                break;
        }
        for ( float& channel : rgb )
            channel += base;
        return rgb;
    }

    [[nodiscard]] double FootprintExtent( const CellBounds& box )
    {
        return std::max( static_cast<double>( box.MaxX ) - static_cast<double>( box.MinX ),
                         static_cast<double>( box.MaxZ ) - static_cast<double>( box.MinZ ) );
    }

    [[nodiscard]] const IRuntimePartition& RuntimePartitionClass( RuntimePartitionType type )
    {
        static const RuntimePartitionLHGrid     lhGrid{};
        static const RuntimePartitionPersistent persistent{};
        switch ( type )
        {
            case RuntimePartitionType::LHGrid:
                return lhGrid;
            case RuntimePartitionType::Persistent:
                return persistent;
        }
        return lhGrid; // unreachable: every enumerator returns above (MSVC C4715 asks for a return here)
    }

    [[nodiscard]] std::string_view RuntimePartitionTypeName( RuntimePartitionType type )
    {
        switch ( type )
        {
            case RuntimePartitionType::LHGrid:
                return "LHGrid";
            case RuntimePartitionType::Persistent:
                return "Persistent";
        }
        return "LHGrid"; // unreachable: every enumerator returns above (MSVC C4715 asks for a return here)
    }

    [[nodiscard]] const RuntimePartitionLHGridSerialized* CellGridOf( const WorldPartitionSerialized& settings,
                                                                      std::size_t                     partition )
    {
        if ( partition >= settings.RuntimePartitions.size() )
            return nullptr;
        const RuntimePartitionSerialized& desc = settings.RuntimePartitions[partition];
        return RuntimePartitionClass( desc.Type ).CellGrid( desc );
    }

    [[nodiscard]] RuntimeGridsCheck CheckRuntimeGrids( const WorldPartitionSerialized& settings )
    {
        RuntimeGridsCheck                              check;
        const std::vector<RuntimePartitionSerialized>& partitions = settings.RuntimePartitions;
        if ( partitions.empty() )
        {
            check.Issues.push_back(
                 { "WorldPartition.RuntimePartitions", "at least one runtime partition", "none" } );
            check.CellsUsable = false;
        }
        for ( std::size_t index = 0; index < partitions.size(); ++index )
        {
            const RuntimePartitionSerialized& desc  = partitions[index];
            const bool                        named = !desc.Name.empty();
            const bool                        twice =
                 named &&
                 std::any_of( partitions.begin(), partitions.begin() + static_cast<std::ptrdiff_t>( index ),
                              [&]( const RuntimePartitionSerialized& other ) { return other.Name == desc.Name; } );
            if ( !named || twice )
            {
                check.Issues.push_back( { std::format( "WorldPartition.RuntimePartitions[{}].Name", index ),
                                          "a name no other partition of the world states", desc.Name } );
                check.CellsUsable = false;
            }
            RuntimePartitionClass( desc.Type ).CheckSettings( desc, index, check );
        }
        return check;
    }

    namespace Detail
    {
        [[nodiscard]] std::optional<std::pair<int, CellCoord>> PlaceFootprint( const CellBounds& footprint,
                                                                               float cellSize, int levelCount )
        {
            const double extent = FootprintExtent( footprint );
            const double centreX =
                 ( static_cast<double>( footprint.MinX ) + static_cast<double>( footprint.MaxX ) ) * 0.5;
            const double centreZ =
                 ( static_cast<double>( footprint.MinZ ) + static_cast<double>( footprint.MaxZ ) ) * 0.5;
            for ( int level = 0; std::isfinite( extent ) && level < levelCount; ++level )
            {
                const double size = LevelCellSize( cellSize, level );
                if ( size < extent )
                    continue;
                if ( !( std::fabs( centreX / size ) < 2147483647.0 ) ||
                     !( std::fabs( centreZ / size ) < 2147483647.0 ) )
                    return std::nullopt;
                return std::make_pair( level,
                                       CellCoord{ static_cast<std::int32_t>( std::floor( centreX / size ) ),
                                                  static_cast<std::int32_t>( std::floor( centreZ / size ) ) } );
            }
            return std::nullopt;
        }

        [[nodiscard]] std::string RecordName( std::span<const EntityDescriptor> records, std::size_t record )
        {
            const EntityDescriptor& described = records[record];
            const std::string       id =
                 described.Id.has_value() ? std::to_string( *described.Id ) : std::format( "record {}", record );
            return described.Tag.empty() ? std::format( "Entities[{}]", id )
                                         : std::format( "{} ({})", described.Tag, id );
        }

        [[nodiscard]] std::string LayerNames( const WorldPartitionSerialized& settings,
                                              std::span<const std::size_t>    layers )
        {
            std::string names;
            for ( const std::size_t layer : layers )
                names += ( names.empty() ? "" : ", " ) + settings.DataLayers->at( layer ).Name;
            return names.empty() ? std::string( "none" ) : names;
        }

        void ValidateReferences( WorldPartitionPlan& plan, std::span<const EntityDescriptor> records,
                                 const WorldPartitionSerialized& settings, bool gridsUsable,
                                 const std::unordered_map<Common::UUID, std::size_t>& byId,
                                 const std::vector<std::optional<CellBounds>>&        ownFootprint )
        {
            std::vector<std::size_t> compositeOf( records.size(), kNoRecord );
            for ( std::size_t group = 0; group < plan.Composites.size(); ++group )
                for ( const std::size_t member : plan.Composites[group].Members )
                    compositeOf[member] = group;
            std::vector<std::size_t> cellOf( plan.Composites.size(), kNoRecord );
            for ( std::size_t cell = 0; cell < plan.Cells.size(); ++cell )
                for ( const std::size_t group : plan.Cells[cell].Composites )
                    cellOf[group] = cell;
            const auto name = [&]( std::size_t record ) { return RecordName( records, record ); };
            const auto add =
                 [&]( GenerationIssueKind kind, std::size_t record, std::size_t target, std::string text )
            { plan.Generation.push_back( GenerationIssue{ kind, record, target, std::move( text ) } ); };

            for ( const DanglingContainment& dangling : plan.Dangling )
                add( GenerationIssueKind::InvalidReference, dangling.Member, kNoRecord,
                     std::format( "{} names {} {}, which no record of the world is: it is loaded without it",
                                  name( dangling.Member ),
                                  dangling.Relation == Containment::Hierarchy ? "the parent"
                                                                              : "the SocketAttachment.Target",
                                  static_cast<std::uint64_t>( dangling.NamedId ) ) );

            for ( const ContainmentEdge& edge : plan.Containment )
            {
                const PlannedComposite& held = plan.Composites[compositeOf[edge.Member]];
                if ( !gridsUsable || ( held.Reason != AlwaysLoadedReason::Author &&
                                       held.Reason != AlwaysLoadedReason::Component ) )
                    continue;
                if ( records[edge.Member].Reason == AlwaysLoadedReason::None ||
                     records[edge.Holder].Reason != AlwaysLoadedReason::None ||
                     !ownFootprint[edge.Holder].has_value() )
                    continue;
                const std::optional<std::size_t> grid = GridIndexOf( settings, records[edge.Holder].RuntimeGrid );
                if ( !grid.has_value() )
                    continue;
                // A Persistent partition places nothing (what streams on it is always loaded): no cell to name.
                const RuntimePartitionSerialized&       partition = settings.RuntimePartitions[*grid];
                const RuntimePartitionLHGridSerialized* lhGrid =
                     RuntimePartitionClass( partition.Type ).CellGrid( partition );
                if ( lhGrid == nullptr )
                    continue;
                const auto place = PlaceFootprint( *ownFootprint[edge.Holder], lhGrid->CellSize, kMaxGridLevels );
                if ( !place.has_value() )
                    continue;
                PlannedCell would;
                would.Grid       = *grid;
                would.Level      = place->first;
                would.Cell       = place->second;
                would.DataLayers = RuntimeDataLayersOf( settings, held.DataLayers );
                add( GenerationIssueKind::AlwaysLoadedReferencesSpatial, edge.Member, edge.Holder,
                     std::format( "AlwaysLoaded {} references spatially loaded {} (cell {}) through {}: the whole "
                                  "composite is always loaded",
                                  name( edge.Member ), name( edge.Holder ), CellStem( settings, would ),
                                  edge.Relation == Containment::Hierarchy ? "its parent"
                                                                          : "SocketAttachment.Target" ) );
            }

            for ( std::size_t record = 0; record < records.size(); ++record )
                for ( const DescriptorReference& reference : records[record].References )
                {
                    if ( reference.Kind != ReferenceKind::Observation )
                        continue;
                    const std::string through = std::format( "{}.{}", reference.Component, reference.Field );
                    const auto        found   = byId.find( Common::UUID( reference.Target ) );
                    if ( found == byId.end() )
                    {
                        add( GenerationIssueKind::InvalidReference, record, kNoRecord,
                             std::format(
                                  "{} references {} through {}, which no record of the world is: it reads null",
                                  name( record ), reference.Target, through ) );
                        continue;
                    }
                    const std::size_t target = found->second;
                    const std::size_t from   = compositeOf[record];
                    const std::size_t to     = compositeOf[target];
                    if ( from == to )
                        continue;
                    const PlannedComposite& referer    = plan.Composites[from];
                    const PlannedComposite& referenced = plan.Composites[to];
                    if ( referer.Reason != AlwaysLoadedReason::None && cellOf[to] != kNoRecord )
                        add( GenerationIssueKind::AlwaysLoadedReferencesSpatial, record, target,
                             std::format(
                                  "AlwaysLoaded {} references spatially loaded {} (cell {}) through {}: it reads "
                                  "null wherever that cell is not loaded",
                                  name( record ), name( target ), CellStem( settings, plan.Cells[cellOf[to]] ),
                                  through ) );
                    const std::vector<std::size_t> fromLayers =
                         RuntimeDataLayersOf( settings, referer.DataLayers );
                    const std::vector<std::size_t> toLayers =
                         RuntimeDataLayersOf( settings, referenced.DataLayers );
                    if ( fromLayers == toLayers )
                        continue;
                    if ( !fromLayers.empty() && toLayers.empty() )
                        add( GenerationIssueKind::ReferenceIntoNoDataLayer, record, target,
                             std::format( "{} in data layers {} references {}, which is in no runtime data layer, "
                                          "through {}",
                                          name( record ), LayerNames( settings, fromLayers ), name( target ),
                                          through ) );
                    else
                        add( GenerationIssueKind::ReferenceAcrossDataLayers, record, target,
                             std::format(
                                  "{} in data layers {} references {} in data layers {} through {}: it reads null "
                                  "while those layers are not activated",
                                  name( record ), LayerNames( settings, fromLayers ), name( target ),
                                  LayerNames( settings, toLayers ), through ) );
                }

            for ( const PlannedComposite& held : plan.Composites )
            {
                // A composite whose anchor names a grid the world lacks has no grid (step 4b's Issue): no member
                // can differ from it.
                if ( !gridsUsable || held.Reason == AlwaysLoadedReason::NoGrid ||
                     !GridIndexOf( settings, records[held.Anchor].RuntimeGrid ).has_value() )
                    continue;
                for ( const std::size_t member : held.Members )
                {
                    if ( member == held.Anchor || !records[member].RuntimeGrid.has_value() )
                        continue;
                    const std::optional<std::size_t> own = GridIndexOf( settings, records[member].RuntimeGrid );
                    if ( own.has_value() && *own == held.Grid )
                        continue;
                    add( GenerationIssueKind::ReferenceAcrossRuntimeGrids, member, held.Anchor,
                         std::format(
                              "{} states runtime grid '{}' but is part of the composite of {}, which streams on "
                              "grid '{}': its own grid is not used",
                              name( member ), *records[member].RuntimeGrid, name( held.Anchor ),
                              settings.RuntimePartitions[held.Grid].Name ) );
                }
            }
        }

    } // namespace Detail
    [[nodiscard]] WorldPartitionPlan PlanWorldPartition( std::span<const EntityDescriptor> records,
                                                         const WorldPartitionSerialized&   settings,
                                                         const AssetBoundsSource&          bounds,
                                                         const ScriptReferenceSource&      scripts )
    {
        WorldPartitionPlan                            plan;
        std::unordered_map<Common::UUID, std::size_t> byId;
        for ( std::size_t record = 0; record < records.size(); ++record )
        {
            if ( records[record].Id.has_value() )
                byId.emplace( Common::UUID( *records[record].Id ), record );
            plan.Issues.insert( plan.Issues.end(), records[record].Issues.begin(), records[record].Issues.end() );
        }

        // ── 1. World positions ────────────────────────────────────────────────────────────────────
        const std::vector<glm::mat4> world = Detail::ComposeWorld( records, byId );

        // A record a registered hook places (a landscape tile's rectangle from its root, a spline's band): after
        // step 1, because a frame record's origin is its WORLD position.
        std::vector<std::optional<Common::Math::AABB>> hookBox( records.size() );
        for ( std::size_t record = 0; record < records.size(); ++record )
        {
            if ( records[record].Facts.empty() )
                continue;
            const Detail::HookedFootprint hooked = Detail::HookFootprintOf( record, records, world, byId );
            if ( !hooked.Hooked )
                continue;
            hookBox[record] = hooked.Box;
            if ( !hooked.Box.has_value() )
                plan.UnplacedFramedRecords.push_back( record );
        }

        // ── 2. Containment edges, then composites ─────────────────────────────────────────────────
        // Every reference resolved once, in record order: a record's edges are taken as they always were - its
        // parent, then its references (the register's, its scripts' hard ones, its names').
        const std::vector<ResolvedReference> references = ResolveEntityReferences( records, scripts, plan.Issues );
        std::size_t                          nextReference = 0;
        for ( const ResolvedReference& reference : references )
            if ( reference.Kind == ReferenceKind::Observation && reference.To != kNoRecord &&
                 reference.To != reference.From )
                plan.Observations.emplace_back( reference.From, reference.To );
        for ( std::size_t record = 0; record < records.size(); ++record )
        {
            const EntityDescriptor& data = records[record];

            // Placed like any record, by the transform it states (v37) and the prefab's box from the
            // registry (Meshes). One that states none is REFUSED by name, not put at the origin.
            if ( !data.MissingTransform.empty() )
            {
                plan.UnplacedPrefabInstances.push_back( record );
                plan.Issues.push_back(
                     Common::Json::Issue{ data.Id.has_value() ? std::format( "Entities[id={}]", *data.Id )
                                                              : std::format( "Entities[{}]", record ),
                                          "a prefab instance stating its root transform (scene v37)",
                                          std::format( "no {} on the instance of '{}'", data.MissingTransform,
                                                       data.PrefabPath.value_or( "" ) ) } );
            }

            if ( const auto parent = Detail::ParentOf( data ); parent.has_value() )
            {
                const auto found = byId.find( *parent );
                if ( found == byId.end() )
                    plan.Dangling.push_back( DanglingContainment{ record, Containment::Hierarchy, *parent } );
                else if ( found->second != record )
                    plan.Containment.push_back(
                         ContainmentEdge{ found->second, record, Containment::Hierarchy, *parent } );
            }

            for ( ; nextReference < references.size() && references[nextReference].From == record;
                  ++nextReference )
            {
                const ResolvedReference& reference = references[nextReference];
                if ( reference.Kind != ReferenceKind::Containment )
                    continue;
                const Containment relation = ContainmentOf( reference.Origin );
                // A name no record carries has no id to dangle by: ResolveEntityReferences named it.
                if ( reference.To == kNoRecord && reference.TargetId.has_value() )
                    plan.Dangling.push_back( DanglingContainment{ record, relation, *reference.TargetId } );
                else if ( reference.To != kNoRecord && reference.To != record )
                    plan.Containment.push_back(
                         ContainmentEdge{ reference.To, record, relation, reference.TargetRecordId } );
            }
        }

        // Union-find over the containment edges. A composite is a connected component of them, which
        // is the only reading of "parts of one whole" that survives a weapon socketed to a character
        // that is itself a child of a vehicle.
        std::vector<std::size_t> forest( records.size() );
        for ( std::size_t record = 0; record < records.size(); ++record )
            forest[record] = record;

        // A free function rather than a lambda: a recursive lambda needs a y-combinator or a
        // std::function, and the parameter-less multi-line form is one of the three constructs
        // clang-format 18 and 22 disagree about (see the CI note in MEMORY).
        struct Find
        {
            static std::size_t Of( std::vector<std::size_t>& forest, std::size_t node )
            {
                while ( forest[node] != node )
                {
                    forest[node] = forest[forest[node]];
                    node         = forest[node];
                }
                return node;
            }
        };

        for ( const ContainmentEdge& edge : plan.Containment )
        {
            const std::size_t holder = Find::Of( forest, edge.Holder );
            const std::size_t member = Find::Of( forest, edge.Member );
            if ( holder != member )
                forest[member] = holder;
        }

        // ── 2b. Composites, and each one's anchor ──────────────────────────────────────────────────
        std::unordered_map<std::size_t, std::size_t> composite; // root of the set -> index in Composites
        for ( std::size_t record = 0; record < records.size(); ++record )
        {
            const std::size_t root  = Find::Of( forest, record );
            auto              found = composite.find( root );
            if ( found == composite.end() )
            {
                found = composite.emplace( root, plan.Composites.size() ).first;
                plan.Composites.push_back( PlannedComposite{} );
            }
            plan.Composites[found->second].Members.push_back( record );
        }

        // What hangs off something. The anchor is the first member in file order that does not.
        std::vector<bool> hangs( records.size(), false );
        for ( const ContainmentEdge& edge : plan.Containment )
            hangs[edge.Member] = true;

        for ( PlannedComposite& group : plan.Composites )
        {
            group.Anchor = group.Members.front();
            for ( const std::size_t member : group.Members )
            {
                if ( !hangs[member] )
                {
                    group.Anchor = member;
                    break;
                }
            }
        }

        // ── 3. Footprint and always-loaded, per composite ─────────────────────────────────────────
        std::vector<bool> unplaced( records.size(), false );
        for ( const std::size_t record : plan.UnplacedPrefabInstances )
            unplaced[record] = true;
        for ( const std::size_t record : plan.UnplacedFramedRecords )
            unplaced[record] = true;

        std::vector<glm::vec3> points;
        // Each record's own footprint, beside its composite's: where it would stream on its own
        // (ValidateReferences).
        std::vector<std::optional<CellBounds>> ownFootprint( records.size() );
        for ( PlannedComposite& group : plan.Composites )
        {
            for ( const std::size_t member : group.Members )
            {
                const AlwaysLoadedReason reason = records[member].Reason;
                // Author outranks Component, and the first member in file order names it.
                if ( reason != AlwaysLoadedReason::None &&
                     ( group.Reason == AlwaysLoadedReason::None ||
                       ( reason == AlwaysLoadedReason::Author && group.Reason != AlwaysLoadedReason::Author ) ) )
                {
                    group.Reason  = reason;
                    group.Because = member;
                }

                if ( unplaced[member] )
                    continue;

                points.clear();
                if ( const auto& box = hookBox[member]; box.has_value() )
                {
                    // The hook's box and NOT the entity's own position: nothing reads a tile's transform.
                    points.push_back( box->Min );
                    points.push_back( box->Max );
                }
                else if ( !Detail::AppendFootprint( records[member], world[member], bounds, points ) )
                    ++plan.PointOnlyRecords;
                for ( const glm::vec3& point : points )
                {
                    std::optional<CellBounds>& own = ownFootprint[member];
                    if ( !own.has_value() )
                        own = CellBounds{ point.x, point.z, point.x, point.z };
                    own->MinX = std::min( own->MinX, point.x );
                    own->MinZ = std::min( own->MinZ, point.z );
                    own->MaxX = std::max( own->MaxX, point.x );
                    own->MaxZ = std::max( own->MaxZ, point.z );
                }
                for ( const glm::vec3& point : points )
                {
                    if ( !group.Footprint.has_value() )
                    {
                        group.Footprint = CellBounds{ point.x, point.z, point.x, point.z };
                        group.Height    = HeightSpan{ point.y, point.y };
                    }
                    CellBounds& box    = *group.Footprint;
                    box.MinX           = std::min( box.MinX, point.x );
                    box.MinZ           = std::min( box.MinZ, point.z );
                    box.MaxX           = std::max( box.MaxX, point.x );
                    box.MaxZ           = std::max( box.MaxZ, point.z );
                    HeightSpan& height = *group.Height;
                    height.MinY        = std::min( height.MinY, point.y );
                    height.MaxY        = std::max( height.MaxY, point.y );
                }
            }
        }

        // ── 4d. The data layers each composite belongs to: its anchor's (WP-DATA-LAYERS) ─────────────────
        // Before the grids, because it does not depend on them: a world whose grids are unusable still holds its
        // composites in the editor by their layers. A name the world does not state is an Issue with the record's
        // path and is NOT read as some other layer (UE drops an invalid data layer reference the same way and
        // the actor is then in no layer by it). A member that states layers other than its anchor's is an Issue
        // too - the composite moves whole, so it cannot belong to two sets of layers (UE refuses an attachment
        // across data layers in its streaming generation, IStreamingGenerationErrorHandler).
        {
            const Common::Json::Issues layerIssues = CheckDataLayers( settings );
            plan.Issues.insert( plan.Issues.end(), layerIssues.begin(), layerIssues.end() );
            const auto sortedNames = []( std::vector<std::string> names )
            {
                std::sort( names.begin(), names.end() );
                names.erase( std::unique( names.begin(), names.end() ), names.end() );
                return names;
            };
            for ( PlannedComposite& held : plan.Composites )
            {
                const EntityDescriptor& anchor = records[held.Anchor];
                for ( const std::string& name : anchor.DataLayers )
                {
                    const std::optional<std::size_t> layer = DataLayerIndexOf( settings, name );
                    if ( !layer.has_value() )
                    {
                        plan.Issues.push_back(
                             { std::format( "Entities[{}].{}.Layers", held.Anchor, kDataLayersComponent ),
                               "data layer", name, Common::Json::IssueKind::MissingReference } );
                        continue;
                    }
                    held.DataLayers.push_back( *layer );
                }
                std::sort( held.DataLayers.begin(), held.DataLayers.end() );
                held.DataLayers.erase( std::unique( held.DataLayers.begin(), held.DataLayers.end() ),
                                       held.DataLayers.end() );
                const std::vector<std::string> anchorNames = sortedNames( anchor.DataLayers );
                for ( const std::size_t member : held.Members )
                    if ( member != held.Anchor && sortedNames( records[member].DataLayers ) != anchorNames )
                        plan.Issues.push_back(
                             { std::format( "Entities[{}].{}.Layers", member, kDataLayersComponent ),
                               std::format( "the data layers of its composite's anchor Entities[{}]",
                                            held.Anchor ),
                               std::format( "{} layer(s) of its own", records[member].DataLayers.size() ) } );
            }
        }

        // ── 3c. The HLOD layer each composite is built by (WP-SETTINGS-UI, UE AActor::HLODLayer) ────────────
        // Its anchor's HLODLayer block; absent = its grid's layer. A name the world does not state is an Issue
        // with the record's path and the composite is built by its grid's layer (UE resolves an invalid HLOD layer
        // reference to the default the same way) - the Issue is what says so. A member that states another layer
        // than its anchor is an Issue too: the composite is one part of one cell's HLOD.
        for ( PlannedComposite& held : plan.Composites )
        {
            const std::optional<std::string>& wanted = records[held.Anchor].HLODLayer;
            if ( wanted.has_value() )
            {
                held.HLODLayer = HLODLayerIndexOf( settings, *wanted );
                if ( !held.HLODLayer.has_value() )
                    plan.Issues.push_back(
                         { std::format( "Entities[{}].{}.Layer", held.Anchor, kHLODLayerComponent ), "HLOD layer",
                           *wanted, Common::Json::IssueKind::MissingReference } );
            }
            for ( const std::size_t member : held.Members )
                if ( member != held.Anchor && records[member].HLODLayer.has_value() &&
                     records[member].HLODLayer != wanted )
                    plan.Issues.push_back(
                         { std::format( "Entities[{}].{}.Layer", member, kHLODLayerComponent ),
                           std::format( "the HLOD layer of its composite's anchor Entities[{}]", held.Anchor ),
                           *records[member].HLODLayer } );
        }

        // ── 4a. The partitions (WP22, WP-HASH-SET; UE FRuntimePartitionDesc): Rules::CheckRuntimeGrids ──────
        // A partition with no name, a name two partitions state, or settings its class cannot place content with
        // make the partitions unusable AS A WHOLE: which partition a record's name reaches would otherwise be a
        // guess.
        RuntimeGridsCheck gridCheck = CheckRuntimeGrids( settings );
        plan.Issues.insert( plan.Issues.end(), std::make_move_iterator( gridCheck.Issues.begin() ),
                            std::make_move_iterator( gridCheck.Issues.end() ) );
        const std::vector<RuntimePartitionSerialized>& partitions = settings.RuntimePartitions;
        plan.GridLevelCount.assign( partitions.size(), 0 );
        if ( !gridCheck.CellsUsable )
        {
            for ( std::size_t group = 0; group < plan.Composites.size(); ++group )
            {
                if ( plan.Composites[group].Reason == AlwaysLoadedReason::None )
                    plan.Composites[group].Reason = AlwaysLoadedReason::NoGrid;
                plan.AlwaysLoaded.push_back( group );
            }
            Detail::ValidateReferences( plan, records, settings, false, byId, ownFootprint );
            return plan;
        }

        // ── 4b. The partition each composite streams on: its anchor's RuntimeGrid (UE AActor::RuntimeGrid) ──
        // Absent = the first partition. A name the world does not state is NOT read as the first partition (UE
        // does that silently): it is an Issue with the record's path, and the composite has no partition - NoGrid.
        for ( PlannedComposite& held : plan.Composites )
        {
            const std::optional<std::string>& wanted = records[held.Anchor].RuntimeGrid;
            if ( !wanted.has_value() )
                continue;
            const std::optional<std::size_t> found = GridIndexOf( settings, wanted );
            if ( !found.has_value() )
            {
                plan.Issues.push_back( { std::format( "Entities[{}].{}.Grid", held.Anchor, kRuntimeGridComponent ),
                                         "runtime partition", *wanted,
                                         Common::Json::IssueKind::MissingReference } );
                if ( held.Reason == AlwaysLoadedReason::None )
                    held.Reason = AlwaysLoadedReason::NoGrid;
                continue;
            }
            held.Grid = *found;
        }

        // A Streaming Source's Target Grids name the world does not state streams nothing (Rules::TargetsGrid):
        // an Issue with the record's path, as a RuntimeGrid name is - never matched to a partition by a guess.
        for ( std::size_t record = 0; record < records.size(); ++record )
            for ( const std::string& name : records[record].TargetGrids )
                if ( std::none_of( partitions.begin(), partitions.end(),
                                   [&]( const RuntimePartitionSerialized& partition )
                                   { return partition.Name == name; } ) )
                    plan.Issues.push_back(
                         { std::format( "Entities[{}].{}.TargetGrids", record, kStreamingSourceComponent ),
                           "runtime partition", name, Common::Json::IssueKind::MissingReference } );
        // Its Target HLOD Layers the same way: a name no HLOD layer of the world carries streams nothing
        // (Rules::TargetsHLODLayer).
        for ( std::size_t record = 0; record < records.size(); ++record )
            for ( const std::string& name : records[record].TargetHLODLayers )
                if ( !settings.HLODLayers.has_value() ||
                     std::none_of( settings.HLODLayers->begin(), settings.HLODLayers->end(),
                                   [&]( const HLODLayerSerialized& layer ) { return layer.Name == name; } ) )
                    plan.Issues.push_back(
                         { std::format( "Entities[{}].{}.TargetHLODLayers", record, kStreamingSourceComponent ),
                           "HLOD layer", name, Common::Json::IssueKind::MissingReference } );

        // ── 5. Each partition places what streams on it, by its class (UE URuntimePartition::GenerateStreaming)
        // ── In partition order, so the cells come out ordered by partition, then (each class's own order) level,
        // X, Z - the plan does not depend on a hash map's iteration order. Refused prefab instances alone
        // (UnplacedPrefabInstances) have no footprint: no place, so no cell and not always loaded - the origin's
        // cell would be indistinguishable from a right answer, and the loader refuses them anyway.
        std::vector<std::vector<std::size_t>> onPartition( partitions.size() );
        for ( std::size_t group = 0; group < plan.Composites.size(); ++group )
        {
            const PlannedComposite& held = plan.Composites[group];
            if ( held.Reason == AlwaysLoadedReason::None && held.Footprint.has_value() )
                onPartition[held.Grid].push_back( group );
        }
        for ( std::size_t index = 0; index < partitions.size(); ++index )
            RuntimePartitionClass( partitions[index].Type )
                 .GenerateStreaming( settings, partitions[index], index, onPartition[index], plan );

        // ── 6. The always-loaded set, in file order of the anchors, and the highest promotion ─────────────
        for ( std::size_t group = 0; group < plan.Composites.size(); ++group )
        {
            const PlannedComposite& held = plan.Composites[group];
            if ( held.Reason != AlwaysLoadedReason::None )
            {
                plan.AlwaysLoaded.push_back( group );
                continue;
            }
            if ( !held.Footprint.has_value() )
                continue;
            if ( plan.MaxLevelComposite == kNoRecord || held.Level > plan.MaxLevel )
            {
                plan.MaxLevel          = held.Level;
                plan.MaxLevelComposite = group;
            }
        }

        // ── 7. The references, validated as UE's streaming generation validates them (Detail::ValidateReferences)
        // ──
        Detail::ValidateReferences( plan, records, settings, true, byId, ownFootprint );
        return plan;
    }

    [[nodiscard]] WorldPartitionPlan PlanWorldPartition( std::span<const Assets::EntityData> records,
                                                         const WorldPartitionSerialized&     settings,
                                                         const AssetBoundsSource&            bounds,
                                                         const ScriptReferenceSource&        scripts )
    {
        const std::vector<EntityDescriptor> descriptors = DescribeEntities( records );
        return PlanWorldPartition( std::span<const EntityDescriptor>( descriptors ), settings, bounds, scripts );
    }

    [[nodiscard]] std::vector<std::size_t> CellsPerLevel( const WorldPartitionPlan& plan )
    {
        std::vector<std::size_t> perLevel( static_cast<std::size_t>( plan.LevelCount ), 0 );
        for ( const PlannedCell& cell : plan.Cells )
            ++perLevel[static_cast<std::size_t>( cell.Level )];
        return perLevel;
    }

    [[nodiscard]] std::vector<std::size_t> AlwaysLoadedByReason( const WorldPartitionPlan& plan )
    {
        std::vector<std::size_t> byReason( static_cast<std::size_t>( AlwaysLoadedReason::Persistent ) + 1, 0 );
        for ( const std::size_t group : plan.AlwaysLoaded )
            ++byReason[static_cast<std::size_t>( plan.Composites[group].Reason )];
        return byReason;
    }

    [[nodiscard]] std::string SummarisePartition( const WorldPartitionPlan&       plan,
                                                  const WorldPartitionSerialized& settings )
    {
        const auto units = []( float value ) { return std::to_string( std::llround( value ) ); };

        std::string line = std::to_string( plan.Composites.size() ) +
                           " composite(s): " + std::to_string( plan.Cells.size() ) + " cell(s) over " +
                           std::to_string( plan.LevelCount ) + " level(s) [";
        const auto perLevel = CellsPerLevel( plan );
        for ( std::size_t level = 0; level < perLevel.size(); ++level )
            line +=
                 ( level == 0 ? "L" : ", L" ) + std::to_string( level ) + ": " + std::to_string( perLevel[level] );

        const auto byReason = AlwaysLoadedByReason( plan );
        line +=
             "], " + std::to_string( plan.AlwaysLoaded.size() ) + " always-loaded (author " +
             std::to_string( byReason[static_cast<std::size_t>( AlwaysLoadedReason::Author )] ) + ", component " +
             std::to_string( byReason[static_cast<std::size_t>( AlwaysLoadedReason::Component )] ) + ", no fit " +
             std::to_string( byReason[static_cast<std::size_t>( AlwaysLoadedReason::NoFit )] ) + ", no grid " +
             std::to_string( byReason[static_cast<std::size_t>( AlwaysLoadedReason::NoGrid )] ) + ", persistent " +
             std::to_string( byReason[static_cast<std::size_t>( AlwaysLoadedReason::Persistent )] ) +
             "); highest promotion: level " + std::to_string( plan.MaxLevel ) + "; " +
             std::to_string( plan.PointOnlyRecords ) + " record(s) placed by position alone";
        for ( const RuntimePartitionSerialized& partition : settings.RuntimePartitions )
        {
            std::format_to( std::back_inserter( line ), "; partition '{}' ({})", partition.Name,
                            RuntimePartitionTypeName( partition.Type ) );
            if ( const RuntimePartitionLHGridSerialized* grid =
                      RuntimePartitionClass( partition.Type ).CellGrid( partition );
                 grid != nullptr )
                std::format_to( std::back_inserter( line ), ": cell {}, loading range {} world units",
                                units( grid->CellSize ), units( grid->LoadingRange ) );
        }
        return line;
    }

    [[nodiscard]] std::optional<Common::Math::AABB> PrefabBounds( std::span<const Assets::EntityData> records,
                                                                  const Common::UUID&                 root,
                                                                  const AssetBoundsSource&            bounds,
                                                                  Common::Json::Issues&               issues )
    {
        std::unordered_map<Common::UUID, std::size_t> byId;
        for ( std::size_t record = 0; record < records.size(); ++record )
        {
            const auto& id = records[record].id;
            if ( id.has_value() && !id->IsNull() )
                byId.emplace( *id, record );
        }
        const std::vector<glm::mat4> world = Detail::ComposeWorld( records, byId );

        const auto      rootRecord = byId.find( root );
        const glm::mat4 toRoot =
             rootRecord != byId.end() ? glm::inverse( world[rootRecord->second] ) : glm::mat4( 1.0f );

        std::optional<Common::Math::AABB> united;
        for ( std::size_t record = 0; record < records.size(); ++record )
        {
            const glm::mat4 frame = toRoot * world[record];
            Detail::ForEachLocalBox( records[record], bounds, issues,
                                     [&]( const Common::Math::AABB& box )
                                     {
                                         for ( int corner = 0; corner < 8; ++corner )
                                         {
                                             const glm::vec4 local( ( corner & 1 ) != 0 ? box.Max.x : box.Min.x,
                                                                    ( corner & 2 ) != 0 ? box.Max.y : box.Min.y,
                                                                    ( corner & 4 ) != 0 ? box.Max.z : box.Min.z,
                                                                    1.0f );
                                             const glm::vec3 placed( frame * local );
                                             if ( !united.has_value() )
                                                 united = Common::Math::AABB{ placed, placed };
                                             united->Min = glm::min( united->Min, placed );
                                             united->Max = glm::max( united->Max, placed );
                                         }
                                     } );
        }
        return united;
    }

} // namespace Desert::Core::Rules
