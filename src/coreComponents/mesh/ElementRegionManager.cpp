/*
 * ------------------------------------------------------------------------------------------------------------
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 * Copyright (c) 2018-2020 Lawrence Livermore National Security LLC
 * Copyright (c) 2018-2020 The Board of Trustees of the Leland Stanford Junior University
 * Copyright (c) 2018-2020 TotalEnergies
 * Copyright (c) 2019-     GEOSX Contributors
 * All rights reserved
 *
 * See top level LICENSE, COPYRIGHT, CONTRIBUTORS, NOTICE, and ACKNOWLEDGEMENTS files for details.
 * ------------------------------------------------------------------------------------------------------------
 */

#include <map>
#include <vector>

#include "ElementRegionManager.hpp"

#include "common/TimingMacros.hpp"
#include "mesh/mpiCommunications/CommunicationTools.hpp"
#include "SurfaceElementRegion.hpp"
#include "FaceManager.hpp"
#include "constitutive/ConstitutiveManager.hpp"
#include "mesh/MeshManager.hpp"
#include "schema/schemaUtilities.hpp"

namespace geosx
{
using namespace dataRepository;

ElementRegionManager::ElementRegionManager( string const & name, Group * const parent ):
  ObjectManagerBase( name, parent )
{
  setInputFlags( InputFlags::OPTIONAL );
  this->registerGroup< Group >( ElementRegionManager::groupKeyStruct::elementRegionsGroup() );
}

ElementRegionManager::~ElementRegionManager()
{
  // TODO Auto-generated destructor stub
}

void ElementRegionManager::resize( integer_array const & numElements,
                                   string_array const & regionNames,
                                   string_array const & GEOSX_UNUSED_PARAM( elementTypes ) )
{
  localIndex const n_regions = LvArray::integerConversion< localIndex >( regionNames.size());
  for( localIndex reg=0; reg<n_regions; ++reg )
  {
    this->getRegion( reg ).resize( numElements[reg] );
  }
}

void ElementRegionManager::setMaxGlobalIndex()
{
  forElementSubRegions< ElementSubRegionBase >( [this] ( ElementSubRegionBase const & subRegion )
  {
    m_localMaxGlobalIndex = std::max( m_localMaxGlobalIndex, subRegion.maxGlobalIndex() );
  } );

  MpiWrapper::allReduce( &m_localMaxGlobalIndex,
                         &m_maxGlobalIndex,
                         1,
                         MPI_MAX,
                         MPI_COMM_GEOSX );
}



Group * ElementRegionManager::createChild( string const & childKey, string const & childName )
{
  GEOSX_ERROR_IF( !(CatalogInterface::hasKeyName( childKey )),
                  "KeyName ("<<childKey<<") not found in ObjectManager::Catalog" );
  GEOSX_LOG_RANK_0( "Adding Object " << childKey<<" named "<< childName<<" from ObjectManager::Catalog." );

  Group & elementRegions = this->getGroup( ElementRegionManager::groupKeyStruct::elementRegionsGroup() );
  return &elementRegions.registerGroup( childName,
                                        CatalogInterface::factory( childKey, childName, &elementRegions ) );

}

void ElementRegionManager::expandObjectCatalogs()
{
  ObjectManagerBase::CatalogInterface::CatalogType const & catalog = ObjectManagerBase::getCatalog();
  for( ObjectManagerBase::CatalogInterface::CatalogType::const_iterator iter = catalog.begin();
       iter!=catalog.end();
       ++iter )
  {
    string const key = iter->first;
    if( key.find( "ElementRegion" ) != string::npos )
    {
      this->createChild( key, key );
    }
  }
}


void ElementRegionManager::setSchemaDeviations( xmlWrapper::xmlNode schemaRoot,
                                                xmlWrapper::xmlNode schemaParent,
                                                integer documentationType )
{
  xmlWrapper::xmlNode targetChoiceNode = schemaParent.child( "xsd:choice" );
  if( targetChoiceNode.empty() )
  {
    targetChoiceNode = schemaParent.prepend_child( "xsd:choice" );
    targetChoiceNode.append_attribute( "minOccurs" ) = "0";
    targetChoiceNode.append_attribute( "maxOccurs" ) = "unbounded";
  }

  std::set< string > names;
  this->forElementRegions( [&]( ElementRegionBase & elementRegion )
  {
    names.insert( elementRegion.getName() );
  } );

  for( string const & name: names )
  {
    schemaUtilities::SchemaConstruction( getRegion( name ), schemaRoot, targetChoiceNode, documentationType );
  }
}

void ElementRegionManager::generateMesh( CellBlockManagerABC & cellBlockManager )
{
  this->forElementRegions< CellElementRegion, SurfaceElementRegion >( [&]( auto & elemRegion )
  {
    elemRegion.generateMesh( cellBlockManager.getCellBlocks() );
  } );
}

void ElementRegionManager::generateWells( MeshManager & meshManager,
                                          MeshLevel & meshLevel )
{
  NodeManager & nodeManager = meshLevel.getNodeManager();

  // get the offsets to construct local-to-global maps for well nodes and elements
  nodeManager.setMaxGlobalIndex();
  globalIndex const nodeOffsetGlobal = nodeManager.maxGlobalIndex() + 1;
  localIndex const elemOffsetLocal  = this->getNumberOfElements();
  globalIndex const elemOffsetGlobal = MpiWrapper::sum( elemOffsetLocal );

  globalIndex wellElemCount = 0;
  globalIndex wellNodeCount = 0;

  // construct the wells one by one
  forElementRegions< WellElementRegion >( [&]( WellElementRegion & wellRegion )
  {

    // get the global well geometry from the well generator
    string const generatorName = wellRegion.getWellGeneratorName();
    InternalWellGenerator const & wellGeometry =
      meshManager.getGroup< InternalWellGenerator >( generatorName );

    // generate the local data (well elements, nodes, perforations) on this well
    // note: each MPI rank knows the global info on the entire well (constructed earlier in InternalWellGenerator)
    // so we only need node and element offsets to construct the local-to-global maps in each wellElemSubRegion
    wellRegion.generateWell( meshLevel, wellGeometry, nodeOffsetGlobal + wellNodeCount, elemOffsetGlobal + wellElemCount );

    // increment counters with global number of nodes and elements
    wellElemCount += wellGeometry.getNumElements();
    wellNodeCount += wellGeometry.getNumNodes();

    string const & subRegionName = wellRegion.getSubRegionName();
    WellElementSubRegion &
    subRegion = wellRegion.getGroup( ElementRegionBase::viewKeyStruct::elementSubRegions() )
                  .getGroup< WellElementSubRegion >( subRegionName );

    globalIndex const numWellElemsGlobal = MpiWrapper::sum( subRegion.size() );

    GEOSX_ERROR_IF( numWellElemsGlobal != wellGeometry.getNumElements(),
                    "Invalid partitioning in well " << subRegionName );

  } );

  // communicate to rebuild global node info since we modified global ordering
  nodeManager.setMaxGlobalIndex();
}

void ElementRegionManager::buildSets( NodeManager const & nodeManager )
{
  GEOSX_MARK_FUNCTION;

  dataRepository::Group const & nodeSets = nodeManager.sets();

  map< string, array1d< bool > > nodeInSet; // map to contain indicator of whether a node is in a set.
  string_array setNames; // just a holder for the names of the sets

  // loop over all wrappers and fill the nodeIndSet arrays for each set
  for( auto & wrapper: nodeSets.wrappers() )
  {
    string const & name = wrapper.second->getName();
    nodeInSet[name].resize( nodeManager.size() );
    nodeInSet[name].setValues< serialPolicy >( false );

    if( nodeSets.hasWrapper( name ) )
    {
      setNames.emplace_back( name );
      SortedArrayView< localIndex const > const & set = nodeSets.getReference< SortedArray< localIndex > >( name );
      for( localIndex const a: set )
      {
        nodeInSet[name][a] = true;
      }
    }
  }

  this->forElementSubRegions(
    [&]( auto & subRegion ) -> void
  {
    dataRepository::Group & elementSets = subRegion.sets();

    auto const & elemToNodeMap = subRegion.nodeList();

    for( string const & setName: setNames )
    {
      arrayView1d< bool const > const nodeInCurSet = nodeInSet[setName];

      SortedArray< localIndex > & targetSet = elementSets.registerWrapper< SortedArray< localIndex > >( setName ).reference();
      for( localIndex k = 0; k < subRegion.size(); ++k )
      {
        localIndex const numNodes = subRegion.numNodesPerElement( k );

        localIndex elementInSet = true;
        for( localIndex i = 0; i < numNodes; ++i )
        {
          if( !nodeInCurSet( elemToNodeMap[k][i] ) )
          {
            elementInSet = false;
            break;
          }
        }

        if( elementInSet )
        {
          targetSet.insert( k );
        }
      }
    }
  } );
}

int ElementRegionManager::PackSize( string_array const & wrapperNames,
                                    ElementViewAccessor< arrayView1d< localIndex > > const & packList ) const
{
  buffer_unit_type * junk = nullptr;
  return PackPrivate< false >( junk, wrapperNames, packList );
}

int ElementRegionManager::Pack( buffer_unit_type * & buffer,
                                string_array const & wrapperNames,
                                ElementViewAccessor< arrayView1d< localIndex > > const & packList ) const
{
  return PackPrivate< true >( buffer, wrapperNames, packList );
}

template< bool DOPACK >
int
ElementRegionManager::PackPrivate( buffer_unit_type * & buffer,
                                   string_array const & wrapperNames,
                                   ElementViewAccessor< arrayView1d< localIndex > > const & packList ) const
{
  int packedSize = 0;

//  packedSize += Group::Pack( buffer, wrapperNames, {}, 0, 0);

  packedSize += bufferOps::Pack< DOPACK >( buffer, this->getName() );
  packedSize += bufferOps::Pack< DOPACK >( buffer, numRegions() );

  parallelDeviceEvents events;
  for( typename dataRepository::indexType kReg=0; kReg<numRegions(); ++kReg )
  {
    ElementRegionBase const & elemRegion = getRegion( kReg );
    packedSize += bufferOps::Pack< DOPACK >( buffer, elemRegion.getName() );

    packedSize += bufferOps::Pack< DOPACK >( buffer, elemRegion.numSubRegions() );

    elemRegion.forElementSubRegionsIndex< ElementSubRegionBase >(
      [&]( localIndex const esr, ElementSubRegionBase const & subRegion )
    {
      packedSize += bufferOps::Pack< DOPACK >( buffer, subRegion.getName() );

      arrayView1d< localIndex const > const elemList = packList[kReg][esr];
      if( DOPACK )
      {
        packedSize += subRegion.pack( buffer, wrapperNames, elemList, 0, false, events );
      }
      else
      {
        packedSize += subRegion.packSize( wrapperNames, elemList, 0, false, events );
      }
    } );
  }

  waitAllDeviceEvents( events );
  return packedSize;
}


int ElementRegionManager::Unpack( buffer_unit_type const * & buffer,
                                  ElementViewAccessor< arrayView1d< localIndex > > & packList )
{
  return unpackPrivate( buffer, packList );
}

int ElementRegionManager::Unpack( buffer_unit_type const * & buffer,
                                  ElementReferenceAccessor< array1d< localIndex > > & packList )
{
  return unpackPrivate( buffer, packList );
}

template< typename T >
int ElementRegionManager::unpackPrivate( buffer_unit_type const * & buffer,
                                         T & packList )
{
  int unpackedSize = 0;

  string name;
  unpackedSize += bufferOps::Unpack( buffer, name );

  GEOSX_ERROR_IF( name != this->getName(), "Unpacked name (" << name << ") does not equal object name (" << this->getName() << ")" );

  localIndex numRegionsRead;
  unpackedSize += bufferOps::Unpack( buffer, numRegionsRead );

  parallelDeviceEvents events;
  for( localIndex kReg=0; kReg<numRegionsRead; ++kReg )
  {
    string regionName;
    unpackedSize += bufferOps::Unpack( buffer, regionName );

    ElementRegionBase & elemRegion = getRegion( regionName );

    localIndex numSubRegionsRead;
    unpackedSize += bufferOps::Unpack( buffer, numSubRegionsRead );
    elemRegion.forElementSubRegionsIndex< ElementSubRegionBase >(
      [&]( localIndex const esr, ElementSubRegionBase & subRegion )
    {
      string subRegionName;
      unpackedSize += bufferOps::Unpack( buffer, subRegionName );

      /// THIS IS WRONG??
      arrayView1d< localIndex > & elemList = packList[kReg][esr];

      unpackedSize += subRegion.unpack( buffer, elemList, 0, false, events );
    } );
  }

  waitAllDeviceEvents( events );
  return unpackedSize;
}

int ElementRegionManager::PackGlobalMapsSize( ElementViewAccessor< arrayView1d< localIndex > > const & packList ) const
{
  buffer_unit_type * junk = nullptr;
  return PackGlobalMapsPrivate< false >( junk, packList );
}

int ElementRegionManager::PackGlobalMaps( buffer_unit_type * & buffer,
                                          ElementViewAccessor< arrayView1d< localIndex > > const & packList ) const
{
  return PackGlobalMapsPrivate< true >( buffer, packList );
}
template< bool DOPACK >
int
ElementRegionManager::PackGlobalMapsPrivate( buffer_unit_type * & buffer,
                                             ElementViewAccessor< arrayView1d< localIndex > > const & packList ) const
{
  int packedSize = 0;

  packedSize += bufferOps::Pack< DOPACK >( buffer, numRegions() );

  for( typename dataRepository::indexType kReg=0; kReg<numRegions(); ++kReg )
  {
    ElementRegionBase const & elemRegion = getRegion( kReg );
    packedSize += bufferOps::Pack< DOPACK >( buffer, elemRegion.getName() );

    packedSize += bufferOps::Pack< DOPACK >( buffer, elemRegion.numSubRegions() );
    elemRegion.forElementSubRegionsIndex< ElementSubRegionBase >(
      [&]( localIndex const esr, ElementSubRegionBase const & subRegion )
    {
      packedSize += bufferOps::Pack< DOPACK >( buffer, subRegion.getName() );

      arrayView1d< localIndex const > const elemList = packList[kReg][esr];
      if( DOPACK )
      {
        packedSize += subRegion.packGlobalMaps( buffer, elemList, 0 );
      }
      else
      {
        packedSize += subRegion.packGlobalMapsSize( elemList, 0 );
      }
    } );
  }

  return packedSize;
}


int
ElementRegionManager::UnpackGlobalMaps( buffer_unit_type const * & buffer,
                                        ElementViewAccessor< ReferenceWrapper< localIndex_array > > & packList )
{
  int unpackedSize = 0;

  localIndex numRegionsRead;
  unpackedSize += bufferOps::Unpack( buffer, numRegionsRead );

  packList.resize( numRegionsRead );
  for( localIndex kReg=0; kReg<numRegionsRead; ++kReg )
  {
    string regionName;
    unpackedSize += bufferOps::Unpack( buffer, regionName );

    ElementRegionBase & elemRegion = getRegion( regionName );

    localIndex numSubRegionsRead;
    unpackedSize += bufferOps::Unpack( buffer, numSubRegionsRead );
    packList[kReg].resize( numSubRegionsRead );
    elemRegion.forElementSubRegionsIndex< ElementSubRegionBase >(
      [&]( localIndex const esr, ElementSubRegionBase & subRegion )
    {
      string subRegionName;
      unpackedSize += bufferOps::Unpack( buffer, subRegionName );

      /// THIS IS WRONG
      localIndex_array & elemList = packList[kReg][esr].get();

      unpackedSize += subRegion.unpackGlobalMaps( buffer, elemList, 0 );
    } );
  }

  return unpackedSize;
}



int ElementRegionManager::PackUpDownMapsSize( ElementViewAccessor< arrayView1d< localIndex > > const & packList ) const
{
  buffer_unit_type * junk = nullptr;
  return packUpDownMapsPrivate< false >( junk, packList );
}
int ElementRegionManager::PackUpDownMapsSize( ElementReferenceAccessor< array1d< localIndex > > const & packList ) const
{
  buffer_unit_type * junk = nullptr;
  return packUpDownMapsPrivate< false >( junk, packList );
}

int ElementRegionManager::PackUpDownMaps( buffer_unit_type * & buffer,
                                          ElementViewAccessor< arrayView1d< localIndex > > const & packList ) const
{
  return packUpDownMapsPrivate< true >( buffer, packList );
}
int ElementRegionManager::PackUpDownMaps( buffer_unit_type * & buffer,
                                          ElementReferenceAccessor< array1d< localIndex > > const & packList ) const
{
  return packUpDownMapsPrivate< true >( buffer, packList );
}

template< bool DOPACK, typename T >
int
ElementRegionManager::packUpDownMapsPrivate( buffer_unit_type * & buffer,
                                             T const & packList ) const
{
  int packedSize = 0;

  packedSize += bufferOps::Pack< DOPACK >( buffer, numRegions() );

  for( typename dataRepository::indexType kReg=0; kReg<numRegions(); ++kReg )
  {
    ElementRegionBase const & elemRegion = getRegion( kReg );
    packedSize += bufferOps::Pack< DOPACK >( buffer, elemRegion.getName() );

    packedSize += bufferOps::Pack< DOPACK >( buffer, elemRegion.numSubRegions() );
    elemRegion.forElementSubRegionsIndex< ElementSubRegionBase >(
      [&]( localIndex const esr, ElementSubRegionBase const & subRegion )
    {
      packedSize += bufferOps::Pack< DOPACK >( buffer, subRegion.getName() );

      arrayView1d< localIndex > const elemList = packList[kReg][esr];
      if( DOPACK )
      {
        packedSize += subRegion.packUpDownMaps( buffer, elemList );
      }
      else
      {
        packedSize += subRegion.packUpDownMapsSize( elemList );
      }
    } );
  }

  return packedSize;
}
//template int
//ElementRegionManager::
//PackUpDownMapsPrivate<true>( buffer_unit_type * & buffer,
//                             ElementViewAccessor<arrayView1d<localIndex>> const & packList ) const;
//template int
//ElementRegionManager::
//PackUpDownMapsPrivate<false>( buffer_unit_type * & buffer,
//                             ElementViewAccessor<arrayView1d<localIndex>> const & packList ) const;


int
ElementRegionManager::UnpackUpDownMaps( buffer_unit_type const * & buffer,
                                        ElementReferenceAccessor< localIndex_array > & packList,
                                        bool const overwriteMap )
{
  int unpackedSize = 0;

  localIndex numRegionsRead;
  unpackedSize += bufferOps::Unpack( buffer, numRegionsRead );

  for( localIndex kReg=0; kReg<numRegionsRead; ++kReg )
  {
    string regionName;
    unpackedSize += bufferOps::Unpack( buffer, regionName );

    ElementRegionBase & elemRegion = getRegion( regionName );

    localIndex numSubRegionsRead;
    unpackedSize += bufferOps::Unpack( buffer, numSubRegionsRead );
    elemRegion.forElementSubRegionsIndex< ElementSubRegionBase >(
      [&]( localIndex const kSubReg, ElementSubRegionBase & subRegion )
    {
      string subRegionName;
      unpackedSize += bufferOps::Unpack( buffer, subRegionName );

      /// THIS IS WRONG
      localIndex_array & elemList = packList[kReg][kSubReg];
      unpackedSize += subRegion.unpackUpDownMaps( buffer, elemList, false, overwriteMap );
    } );
  }

  return unpackedSize;
}

int ElementRegionManager::packFracturedElementsSize( ElementViewAccessor< arrayView1d< localIndex > > const & packList,
                                                     string const fractureRegionName ) const
{
  buffer_unit_type * junk = nullptr;
  return packFracturedElementsPrivate< false >( junk, packList, fractureRegionName );
}

int ElementRegionManager::packFracturedElements( buffer_unit_type * & buffer,
                                                 ElementViewAccessor< arrayView1d< localIndex > > const & packList,
                                                 string const fractureRegionName ) const
{
  return packFracturedElementsPrivate< true >( buffer, packList, fractureRegionName );
}
template< bool DOPACK >
int
ElementRegionManager::packFracturedElementsPrivate( buffer_unit_type * & buffer,
                                                    ElementViewAccessor< arrayView1d< localIndex > > const & packList,
                                                    string const fractureRegionName ) const
{
  int packedSize = 0;

  SurfaceElementRegion const & embeddedSurfaceRegion =
    this->getRegion< SurfaceElementRegion >( fractureRegionName );
  EmbeddedSurfaceSubRegion const & embeddedSurfaceSubRegion =
    embeddedSurfaceRegion.getSubRegion< EmbeddedSurfaceSubRegion >( 0 );

  arrayView1d< globalIndex const > const embeddedSurfacesLocalToGlobal =
    embeddedSurfaceSubRegion.localToGlobalMap();

  packedSize += bufferOps::Pack< DOPACK >( buffer, numRegions() );

  for( typename dataRepository::indexType kReg=0; kReg<numRegions(); ++kReg )
  {
    ElementRegionBase const & elemRegion = getRegion( kReg );
    packedSize += bufferOps::Pack< DOPACK >( buffer, elemRegion.getName() );

    packedSize += bufferOps::Pack< DOPACK >( buffer, elemRegion.numSubRegions() );
    elemRegion.forElementSubRegionsIndex< CellElementSubRegion >(
      [&]( localIndex const esr, CellElementSubRegion const & subRegion )
    {
      packedSize += bufferOps::Pack< DOPACK >( buffer, subRegion.getName() );

      arrayView1d< localIndex const > const elemList = packList[kReg][esr];
      if( DOPACK )
      {
        packedSize += subRegion.packFracturedElements( buffer, elemList, embeddedSurfacesLocalToGlobal );
      }
      else
      {
        packedSize += subRegion.packFracturedElementsSize( elemList, embeddedSurfacesLocalToGlobal );
      }
    } );
  }

  return packedSize;
}

int
ElementRegionManager::unpackFracturedElements( buffer_unit_type const * & buffer,
                                               ElementReferenceAccessor< localIndex_array > & packList,
                                               string const fractureRegionName )
{
  int unpackedSize = 0;

  SurfaceElementRegion & embeddedSurfaceRegion =
    this->getRegion< SurfaceElementRegion >( fractureRegionName );
  EmbeddedSurfaceSubRegion & embeddedSurfaceSubRegion =
    embeddedSurfaceRegion.getSubRegion< EmbeddedSurfaceSubRegion >( 0 );

  unordered_map< globalIndex, localIndex > embeddedSurfacesGlobalToLocal =
    embeddedSurfaceSubRegion.globalToLocalMap();

  localIndex numRegionsRead;
  unpackedSize += bufferOps::Unpack( buffer, numRegionsRead );

  for( localIndex kReg=0; kReg<numRegionsRead; ++kReg )
  {
    string regionName;
    unpackedSize += bufferOps::Unpack( buffer, regionName );

    ElementRegionBase & elemRegion = getRegion( regionName );

    localIndex numSubRegionsRead;
    unpackedSize += bufferOps::Unpack( buffer, numSubRegionsRead );
    elemRegion.forElementSubRegionsIndex< CellElementSubRegion >(
      [&]( localIndex const kSubReg, CellElementSubRegion & subRegion )
    {
      string subRegionName;
      unpackedSize += bufferOps::Unpack( buffer, subRegionName );

      /// THIS IS WRONG
      localIndex_array & elemList = packList[kReg][kSubReg];
      unpackedSize += subRegion.unpackFracturedElements( buffer, elemList, embeddedSurfacesGlobalToLocal );
    } );
  }

  return unpackedSize;
}


REGISTER_CATALOG_ENTRY( ObjectManagerBase, ElementRegionManager, string const &, Group * const )
}
