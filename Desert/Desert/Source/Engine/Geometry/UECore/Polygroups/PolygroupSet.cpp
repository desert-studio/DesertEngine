// Ported from UE 5.8
// Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Private/Polygroups/PolygroupSet.cpp:1-146, adapted:
// UE Core via UECore.hpp, namespace Desert::Geometry, TUniquePtr is std::unique_ptr; std::string layer lookup by
// std::string.
#include "Engine/Geometry/UECore/Polygroups/PolygroupSet.hpp"

using namespace Desert::Geometry;

// Ported from UE 5.8
// Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Private/Polygroups/PolygroupUtil.cpp: 7-78 (the
// polygroup-layer lookups FPolygroupSet uses), adapted: FName is std::string; the triangle-label lookups are not
// ported.
namespace
{
    const FDynamicMeshPolygroupAttribute* FindPolygroupLayerByName( const FDynamicMesh3& Mesh, std::string Name )
    {
        const FDynamicMeshAttributeSet* AttributeSet = Mesh.Attributes();
        if ( AttributeSet == nullptr )
            return nullptr;
        int32_t NumPolygroupLayers = AttributeSet->NumPolygroupLayers();
        for ( int32_t k = 0; k < NumPolygroupLayers; ++k )
        {
            if ( AttributeSet->GetPolygroupLayer( k )->GetName() == Name )
            {
                return AttributeSet->GetPolygroupLayer( k );
            }
        }
        return nullptr;
    }

    int32_t FindPolygroupLayerIndex( const FDynamicMesh3& Mesh, const FDynamicMeshPolygroupAttribute* Layer )
    {
        const FDynamicMeshAttributeSet* AttributeSet = Mesh.Attributes();
        if ( AttributeSet == nullptr )
            return -1;
        int32_t NumPolygroupLayers = AttributeSet->NumPolygroupLayers();
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

bool FPolygroupLayer::CheckExists( const FDynamicMesh3* Mesh ) const
{
    if ( Mesh )
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

void FPolygroupLayer::EnableOnMesh( FDynamicMesh3& Mesh ) const
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

FPolygroupSet::FPolygroupSet( const FPolygroupSet* CopyIn )
{
    Mesh            = CopyIn->Mesh;
    PolygroupAttrib = CopyIn->PolygroupAttrib;
    GroupLayerIndex = CopyIn->GroupLayerIndex;
    MaxGroupID      = CopyIn->MaxGroupID;
}

FPolygroupSet::FPolygroupSet( const FDynamicMesh3* MeshIn )
{
    Mesh            = MeshIn;
    GroupLayerIndex = -1;
    RecalculateMaxGroupID();
}

/** Initialize a PolygroupSet for the given Mesh, and standard triangle group layer */
FPolygroupSet::FPolygroupSet( const FDynamicMesh3* MeshIn, FPolygroupLayer GroupLayer )
{
    Mesh            = MeshIn;
    GroupLayerIndex = -1;
    if ( !GroupLayer.bIsDefaultLayer )
    {
        if ( UE_ENSURE( Mesh->Attributes() ) )
        {
            if ( GroupLayer.LayerIndex < Mesh->Attributes()->NumPolygroupLayers() )
            {
                PolygroupAttrib = Mesh->Attributes()->GetPolygroupLayer( GroupLayer.LayerIndex );
                GroupLayerIndex = GroupLayer.LayerIndex;
            }
        }
        if ( GroupLayerIndex == -1 )
        {
            UE_ENSURE_MSGF( false, "FPolygroupSet: Attribute index missing!" );
        }
    }
    RecalculateMaxGroupID();
}

FPolygroupSet::FPolygroupSet( const FDynamicMesh3*                  MeshIn,
                              const FDynamicMeshPolygroupAttribute* PolygroupAttribIn )
{
    Mesh            = MeshIn;
    PolygroupAttrib = PolygroupAttribIn;
    GroupLayerIndex = FindPolygroupLayerIndex( *MeshIn, PolygroupAttrib );
    RecalculateMaxGroupID();
}

FPolygroupSet::FPolygroupSet( const FDynamicMesh3* MeshIn, int32_t PolygroupLayerIndex )
{
    Mesh = MeshIn;
    if ( UE_ENSURE( Mesh->Attributes() ) )
    {
        if ( PolygroupLayerIndex < Mesh->Attributes()->NumPolygroupLayers() )
        {
            PolygroupAttrib = Mesh->Attributes()->GetPolygroupLayer( PolygroupLayerIndex );
            GroupLayerIndex = PolygroupLayerIndex;
            return;
        }
    }
    RecalculateMaxGroupID();
    UE_ENSURE_MSGF( false, "FPolygroupSet: Attribute index missing!" );
}

FPolygroupSet::FPolygroupSet( const FDynamicMesh3* MeshIn, std::string AttribName )
{
    Mesh            = MeshIn;
    PolygroupAttrib = FindPolygroupLayerByName( *MeshIn, AttribName );
    GroupLayerIndex = FindPolygroupLayerIndex( *MeshIn, PolygroupAttrib );
    RecalculateMaxGroupID();
    UE_ENSURE_MSGF( PolygroupAttrib != nullptr, "FPolygroupSet: Attribute set missing!" );
}

void FPolygroupSet::RecalculateMaxGroupID()
{
    MaxGroupID = 0;
    if ( PolygroupAttrib )
    {
        for ( int32_t tid : Mesh->TriangleIndicesItr() )
        {
            MaxGroupID = std::max( MaxGroupID, PolygroupAttrib->GetValue( tid ) + 1 );
        }
    }
    else
    {
        for ( int32_t tid : Mesh->TriangleIndicesItr() )
        {
            MaxGroupID = std::max( MaxGroupID, Mesh->GetTriangleGroup( tid ) + 1 );
        }
    }
}