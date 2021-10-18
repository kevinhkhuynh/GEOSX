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

/**
 * @file EzrokhiBrineDensity.cpp
 */

#include "constitutive/fluid/PVTFunctions/EzrokhiBrineDensity.hpp"

#include "constitutive/fluid/PVTFunctions/PVTFunctionHelpers.hpp"
#include "functions/FunctionManager.hpp"

namespace geosx
{

using namespace stringutilities;

namespace constitutive
{

namespace PVTProps
{

namespace
{

TableFunction const * makeDensityTable( string const & functionName,
                                        FunctionManager & functionManager )
{
  array1d< array1d< real64 > > temperatures;
  array1d< real64 > densities;

  temperatures.resize( 1 );
  temperatures[0].resize( 26 );
  densities.resize( 26 );

  temperatures[0][0] = 0.01;
  temperatures[0][1] = 10;
  temperatures[0][2] = 20;
  temperatures[0][3] = 25;
  temperatures[0][4] = 30;
  temperatures[0][5] = 40;
  temperatures[0][6] = 50;
  temperatures[0][7] = 60;
  temperatures[0][8] = 70;
  temperatures[0][9] = 80;
  temperatures[0][10] = 90;
  temperatures[0][11] = 100;
  temperatures[0][12] = 110;
  temperatures[0][13] = 120;
  temperatures[0][14] = 140;
  temperatures[0][15] = 160;
  temperatures[0][16] = 180;
  temperatures[0][17] = 200;
  temperatures[0][18] = 220;
  temperatures[0][19] = 240;
  temperatures[0][20] = 260;
  temperatures[0][21] = 280;
  temperatures[0][22] = 300;
  temperatures[0][23] = 320;
  temperatures[0][24] = 340;
  temperatures[0][25] = 360;


  densities[0] = 999.85;
  densities[1] = 999.7;
  densities[2] = 998.21;
  densities[3] = 997.05;
  densities[4] = 995.65;
  densities[5] = 995.65;
  densities[6] = 988.04;
  densities[7] = 983.2;
  densities[8] = 977.76;
  densities[9] = 971.79;
  densities[10] = 965.31;
  densities[11] = 958.35; // this value is used as referense density with referense pressure 100 kPa.
  densities[12] = 950.95;
  densities[13] = 943.11;
  densities[14] = 926.13;
  densities[15] = 907.45;
  densities[16] = 887;
  densities[17] = 864.66;
  densities[18] = 840.22;
  densities[19] = 813.37;
  densities[20] = 783.63;
  densities[21] = 750.28;
  densities[22] = 712.14;
  densities[23] = 667.09;
  densities[24] = 610.67;
  densities[25] = 527.59;

  string const tableName = functionName +  "_table";
  if( functionManager.hasGroup< TableFunction >( tableName ) )
  {
    return functionManager.getGroupPointer< TableFunction >( tableName );
  }
  else
  {
    TableFunction * const densityTable = dynamicCast< TableFunction * >( functionManager.createChild( "TableFunction", tableName ) );
    densityTable->setTableCoordinates( temperatures );
    densityTable->setTableValues( densities );
    densityTable->setInterpolationMethod( TableFunction::InterpolationType::Linear );
    return densityTable;
  }
}

} // namespace


EzrokhiBrineDensity::EzrokhiBrineDensity( string const & name,
                                          string_array const & inputPara,
                                          string_array const & componentNames,
                                          array1d< real64 > const & componentMolarWeight ):
  PVTFunctionBase( name,
                   componentNames,
                   componentMolarWeight )
{
  string const expectedCO2ComponentNames[] = { "CO2", "co2" };
  m_CO2Index = PVTFunctionHelpers::findName( componentNames, expectedCO2ComponentNames, "componentNames" );

  string const expectedWaterComponentNames[] = { "Water", "water" };
  m_waterIndex = PVTFunctionHelpers::findName( componentNames, expectedWaterComponentNames, "componentNames" );

  makeCoefficients( inputPara );
  m_waterDensityTable = makeDensityTable( m_functionName, FunctionManager::getInstance() );
}

void EzrokhiBrineDensity::makeCoefficients( string_array const & inputPara )
{
  // compute brine density following Ezrokhi`s method (referenced in Eclipse TD, Aqueous phase properties)
  // Reference : Zaytsev, I.D. and Aseyev, G.G. Properties of Aqueous Solutions of Electrolytes, Boca Raton, Florida, USA CRC Press (1993).

  m_waterCompressibility = 4.5e-10; // Pa-1
  m_waterRefDensity = 958.35;
  m_waterRefPressure = 1e5;
  GEOSX_THROW_IF_LT_MSG( inputPara.size(), 5,
                         GEOSX_FMT( "{}: insufficient number of model parameters", m_functionName ),
                         InputError );

  try
  {
    // assume CO2 is the only non-water component in the brine
    m_coef0 = stod( inputPara[2] );
    m_coef1 = stod( inputPara[3] );
    m_coef2 = stod( inputPara[4] );
  }
  catch( std::invalid_argument const & e )
  {
    GEOSX_THROW( GEOSX_FMT( "{}: invalid model parameter value '{}'", m_functionName, e.what() ), InputError );
  }
}

EzrokhiBrineDensity::KernelWrapper
EzrokhiBrineDensity::createKernelWrapper() const
{
  return KernelWrapper( m_componentMolarWeight,
                        *m_waterDensityTable,
                        m_CO2Index,
                        m_waterIndex,
                        m_waterCompressibility,
                        m_waterRefDensity,
                        m_waterRefPressure,
                        m_coef0,
                        m_coef1,
                        m_coef2 );
}

REGISTER_CATALOG_ENTRY( PVTFunctionBase, EzrokhiBrineDensity, string const &, string_array const &, string_array const &, array1d< real64 > const & )

} // end namespace PVTProps

} // namespace constitutive

} // end namespace geosx
