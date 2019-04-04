
/*
 *~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 * Copyright (c) 2019, Lawrence Livermore National Security, LLC.
 *
 * Produced at the Lawrence Livermore National Laboratory
 *
 * LLNL-CODE-746361
 *
 * All rights reserved. See COPYRIGHT for details.
 *
 * This file is part of the GEOSX Simulation Framework.
 *
 * GEOSX is a free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License (as published by the
 * Free Software Foundation) version 2.1 dated February 1999.
 *~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */

/**
 * @file FenghourCO2ViscosityFunction.hpp
 */

#ifndef SRC_COMPONENTS_CORE_SRC_CONSTITUTIVE_FENGHOURCO2VISCOSITYFUNCTION_HPP
#define SRC_COMPONENTS_CORE_SRC_CONSTITUTIVE_FENGHOURCO2VISCOSITYFUNCTION_HPP

#include "PVTFunctionBase.hpp"

namespace geosx
{

namespace PVTProps
{

class FenghourCO2ViscosityFunction : public PVTFunctionBase
{
public:

  FenghourCO2ViscosityFunction( const string_array& inputPara,
                                const string_array& componentNames,
                                const real64_array& componentMolarWeight );
  ~FenghourCO2ViscosityFunction() override
      {}

  static constexpr auto m_catalogName = "FenghourCO2Viscosity";
  static string CatalogName()                    { return m_catalogName; }
  virtual string GetCatalogName() override final { return CatalogName(); }

  virtual PVTFUNCTYPE FunctionType() const override
  {
    return PVTFUNCTYPE::VISCOSITY;

  }

  virtual void Evaluation( const EvalVarArgs& pressure,
                           const EvalVarArgs& temperature,
                           const array1dT<EvalVarArgs>& phaseComposition,
                           EvalVarArgs& value, bool useMass = 0) const override;


private:

  void MakeTable(const string_array& inputPara);

  void CalculateCO2Viscosity(const real64_vector& pressure, const real64_vector& temperature, const array1dT<real64_vector>& density, array1dT<real64_vector>& viscosity);

  void FenghourCO2Viscosity(real64 const Tcent, real64 const den, real64 &vis);

  TableFunctionPtr m_CO2ViscosityTable;
};

}

}
#endif
