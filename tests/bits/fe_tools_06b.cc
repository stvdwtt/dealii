//----------------------------  fe_tools_06b.cc  ---------------------------
//    $Id$
//    Version: $Name$ 
//
//    Copyright (C) 2003, 2004, 2013 by the deal.II authors
//
//    This file is subject to QPL and may not be  distributed
//    without copyright and license information. Please refer
//    to the file deal.II/doc/license.html for the  text  and
//    further information on this license.
//
//----------------------------  fe_tools_06b.cc  ---------------------------

#include "../tests.h"
#include "fe_tools_common.h"
#include <deal.II/lac/parallel_vector.h>
#include <deal.II/lac/sparsity_pattern.h>

// check
//   FETools::back_interpolate(6) with parallel vector


std::string output_file_name = "fe_tools_06b/output";


template <int dim>
void
check_this (const FiniteElement<dim> &fe1,
            const FiniteElement<dim> &fe2)
{
                                   // only check if both elements have
                                   // support points. otherwise,
                                   // interpolation doesn't really
                                   // work
  if ((fe1.get_unit_support_points().size() == 0) ||
      (fe2.get_unit_support_points().size() == 0))
    return;
                                   //  likewise for non-primitive elements
  if (!fe1.is_primitive() || !fe2.is_primitive())
    return;
                                   // we need to have dof_constraints
                                   // for this test
  if (!fe1.constraints_are_implemented() ||
      !fe2.constraints_are_implemented())
    return;

  std::auto_ptr<Triangulation<dim> > tria(make_tria<dim>());
  std::auto_ptr<DoFHandler<dim> >    dof1(make_dof_handler (*tria, fe1));
  std::auto_ptr<DoFHandler<dim> >    dof2(make_dof_handler (*tria, fe2));
  ConstraintMatrix cm1, cm2;
  DoFTools::make_hanging_node_constraints (*dof1, cm1);
  DoFTools::make_hanging_node_constraints (*dof2, cm2);
  cm1.close ();
  cm2.close ();
  
  parallel::distributed::Vector<double> in (dof1->n_dofs());
  for (unsigned int i=0; i<in.size(); ++i) in(i) = i;
  parallel::distributed::Vector<double> out (dof1->n_dofs());
  
  FETools::back_interpolate (*dof1, cm1, in, *dof2, cm2, out);
  output_vector (out);
}
