// Ported from UE 5.8
// Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Private/Polygroups/PolygroupSet.cpp:1-146, adapted:
// UE Core via UECore.hpp, namespace Desert::Geometry, TUniquePtr is std::unique_ptr; std::string layer lookup by
// std::string.
#include "Engine/Geometry/UECore/Polygroups/PolygroupSet.hpp"
#include <Common/Core/Core.hpp>

using namespace Desert::Geometry;

// Ported from UE 5.8
// Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Private/Polygroups/PolygroupUtil.cpp: 7-78 (the
// polygroup-layer lookups PolygroupSet uses), adapted: std::string is std::string; the triangle-label lookups are
// not ported.
namespace
{
    const DynamicMeshPolygroupAttribute* FindPolygroupLayerByName( const DynamicMesh3& Mesh,
                                                                   const std::string&  Name )
    {
        const DynamicMeshAttributeSet* AttributeSet = Mesh.Attributes();
        if ( AttributeSet == nullptr )
            return nullptr;
        int32_t const NumPolygroupLayers = AttributeSet->NumPolygroupLayers();
        for ( int32_t k = 0; k < NumPolygroupLayers; ++k )
        {
            if ( AttributeSet->GetPolygroupLayer( k )->GetName() == Name )
            {
                return AttributeSet->GetPolygroupLayer( k );
            }
        }
        return nullptr;
    }

    int32_t FindPolygroupLayerIndex( const DynamicMesh3& Mesh, const DynamicMeshPolygroupAttribute* Layer )
    {
        const DynamicMeshAttributeSet* AttributeSet = Mesh.Attributes();
        if ( AttributeSet == nullptr )
            return -1;
        int32_t const NumPolygroupLayers = AttributeSet->NumPolygroupLayers();
        for ( int32_t k = 0; k < NumPolygroupLayers; ++k )
        {
            if ( AttributeSet->GetPolygroupLayer( k ) == Layer )
            {
                return k;
            }
        }
        return -1;
    }
} // namespace

bool PolygroupLayer::CheckExists( const DynamicMesh3* Mesh ) const
{
    if ( Mesh != nullptr )
    {
        if ( bIsDefaultLayer )
        {
            if ( Mesh->HasTriangleGroups() )
            {
                return true;
            }
        }
        else
        {
            if ( LayerIndex >= 0 && Mesh->HasAttributes() &&
                 LayerIndex < Mesh->Attributes()->NumPolygroupLayers() )
            {
                return true;
            }
        }
    }
    return false;
}

void PolygroupLayer::EnableOnMesh( DynamicMesh3& Mesh ) const
{
    if ( bIsDefaultLayer )
    {
        if ( !Mesh.HasTriangleGroups() )
        {
            Mesh.EnableTriangleGroups();
        }
    }
    else
    {
        if ( !Mesh.HasAttributes() )
        {
            Mesh.EnableAttributes();
        }
        if ( Mesh.Attributes()->NumPolygroupLayers() <= LayerIndex )
        {
            Mesh.Attributes()->SetNumPolygroupLayers( LayerIndex + 1 );
        }
    }
}

PolygroupSet::PolygroupSet( const PolygroupSet* CopyIn )
     : Mesh( CopyIn->Mesh ), PolygroupAttrib( CopyIn->PolygroupAttrib )
{

    GroupLayerIndex = CopyIn->GroupLayerIndex;
    MaxGroupID      = CopyIn->MaxGroupID;
}

PolygroupSet::PolygroupSet( const DynamicMesh3* MeshIn ) : Mesh( MeshIn ), GroupLayerIndex( -1 )
{

    RecalculateMaxGroupID();
}

/** Initialize a PolygroupSet for the given Mesh, and standard triangle group layer */
PolygroupSet::PolygroupSet( const DynamicMesh3* MeshIn, PolygroupLayer GroupLayer )
     : Mesh( MeshIn ), GroupLayerIndex( -1 )
{

    if ( !GroupLayer.bIsDefaultLayer )
    {
        if ( Common::EnsureOrWarn( Mesh->Attributes() != nullptr, "Mesh->Attributes()" ) )
        {
            if ( GroupLayer.LayerIndex < Mesh->Attributes()->NumPolygroupLayers() )
            {
                PolygroupAttrib = Mesh->Attributes()->GetPolygroupLayer( GroupLayer.LayerIndex );
                GroupLayerIndex = GroupLayer.LayerIndex;
            }
        }
        if ( GroupLayerIndex == -1 )
        {
            DESERT_VERIFY_WARN( false, "PolygroupSet: Attribute index missing!" );
        }
    }
    RecalculateMaxGroupID();
}

PolygroupSet::PolygroupSet( const DynamicMesh3* MeshIn, const DynamicMeshPolygroupAttribute* PolygroupAttribIn )
     : Mesh( MeshIn ), PolygroupAttrib( PolygroupAttribIn )
{

    GroupLayerIndex = FindPolygroupLayerIndex( *MeshIn, PolygroupAttrib );
    RecalculateMaxGroupID();
}

PolygroupSet::PolygroupSet( const DynamicMesh3* MeshIn, int32_t PolygroupLayerIndex ) : Mesh( MeshIn )
{

    if ( Common::EnsureOrWarn( Mesh->Attributes() != nullptr, "Mesh->Attributes()" ) )
    {
        if ( PolygroupLayerIndex < Mesh->Attributes()->NumPolygroupLayers() )
        {
            PolygroupAttrib = Mesh->Attributes()->GetPolygroupLayer( PolygroupLayerIndex );
            GroupLayerIndex = PolygroupLayerIndex;
            return;
        }
    }
    RecalculateMaxGroupID();
    DESERT_VERIFY_WARN( false, "PolygroupSet: Attribute index missing!" );
}

PolygroupSet::PolygroupSet( const DynamicMesh3* MeshIn, const std::string& AttribName )
     : Mesh( MeshIn ), PolygroupAttrib( FindPolygroupLayerByName( *MeshIn, AttribName ) )
{

    GroupLayerIndex = FindPolygroupLayerIndex( *MeshIn, PolygroupAttrib );
    RecalculateMaxGroupID();
    DESERT_VERIFY_WARN( PolygroupAttrib != nullptr, "PolygroupSet: Attribute set missing!" );
}

void PolygroupSet::RecalculateMaxGroupID()
{
    MaxGroupID = 0;
    if ( PolygroupAttrib != nullptr )
    {
        for ( int32_t const tid : Mesh->TriangleIndicesItr() )
        {
            MaxGroupID = std::max( MaxGroupID, PolygroupAttrib->GetValue( tid ) + 1 );
        }
    }
    else
    {
        for ( int32_t const tid : Mesh->TriangleIndicesItr() )
        {
            MaxGroupID = std::max( MaxGroupID, Mesh->GetTriangleGroup( tid ) + 1 );
        }
    }
}