// ---------------------------------------------------------------------
// $Id$
//
// Copyright (C) 2009 - 2013 by the deal.II authors
//
// This file is part of the deal.II library.
//
// The deal.II library is free software; you can use it, redistribute
// it, and/or modify it under the terms of the GNU Lesser General
// Public License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.
// The full text of the license can be found in the file LICENSE at
// the top level of the deal.II distribution.
//
// ---------------------------------------------------------------------



// same as assemble_matrix_parallel_01, but now for a BlockSparseMatrix

#include "../tests.h"

#include <deal.II/base/quadrature_lib.h>
#include <deal.II/base/function.h>
#include <deal.II/base/logstream.h>
#include <deal.II/base/utilities.h>
#include <deal.II/base/work_stream.h>
#include <deal.II/base/graph_coloring.h>
#include <deal.II/lac/vector.h>
#include <deal.II/lac/full_matrix.h>
#include <deal.II/lac/block_sparse_matrix.h>
#include <deal.II/lac/block_sparsity_pattern.h>
#include <deal.II/lac/solver_cg.h>
#include <deal.II/lac/precondition.h>
#include <deal.II/grid/tria.h>
#include <deal.II/grid/grid_generator.h>
#include <deal.II/grid/tria_accessor.h>
#include <deal.II/grid/tria_iterator.h>
#include <deal.II/grid/grid_refinement.h>
#include <deal.II/dofs/dof_accessor.h>
#include <deal.II/dofs/dof_tools.h>
#include <deal.II/lac/constraint_matrix.h>
#include <deal.II/fe/fe_q.h>
#include <deal.II/numerics/vector_tools.h>
#include <deal.II/numerics/matrix_tools.h>
#include <deal.II/numerics/error_estimator.h>
#include <deal.II/hp/dof_handler.h>
#include <deal.II/hp/fe_values.h>

#include <fstream>
#include <iostream>
#include <complex>

std::ofstream logfile("output");

using namespace dealii;


namespace Assembly
{
  namespace Scratch
  {
    template <int dim>
    struct Data
    {
      Data (const hp::FECollection<dim> &fe,
            const hp::QCollection<dim>  &quadrature)
        :
        hp_fe_values(fe,
                     quadrature,
                     update_values    |  update_gradients |
                     update_quadrature_points  |  update_JxW_values)
      {}

      Data (const Data &data)
        :
        hp_fe_values(data.hp_fe_values.get_mapping_collection(),
                     data.hp_fe_values.get_fe_collection(),
                     data.hp_fe_values.get_quadrature_collection(),
                     data.hp_fe_values.get_update_flags())
      {}

      hp::FEValues<dim>               hp_fe_values;
    };
  }

  namespace Copy
  {
    struct Data
    {
      std::vector<types::global_dof_index> local_dof_indices;
      FullMatrix<double> local_matrix;
      Vector<double> local_rhs;
    };
  }
}

template <int dim>
class LaplaceProblem
{
public:
  LaplaceProblem ();
  ~LaplaceProblem ();

  void run ();

private:
  void setup_system ();
  void test_equality ();
  void assemble_reference ();
  void assemble_test ();
  void solve ();
  void create_coarse_grid ();
  void postprocess ();

  void local_assemble (const typename hp::DoFHandler<dim>::active_cell_iterator &cell,
                       Assembly::Scratch::Data<dim>  &scratch,
                       Assembly::Copy::Data          &data);
  void copy_local_to_global (const Assembly::Copy::Data &data);

  std::vector<types::global_dof_index>
  get_conflict_indices (typename hp::DoFHandler<dim>::active_cell_iterator const &cell) const;

  Triangulation<dim>   triangulation;

  hp::DoFHandler<dim>      dof_handler;
  hp::FECollection<dim>    fe_collection;
  hp::QCollection<dim>     quadrature_collection;
  hp::QCollection<dim-1>   face_quadrature_collection;

  ConstraintMatrix     constraints;

  BlockSparsityPattern      sparsity_pattern;
  BlockSparseMatrix<double> reference_matrix;
  BlockSparseMatrix<double> test_matrix;

  Vector<double>       solution;
  Vector<double>       reference_rhs;
  Vector<double>       test_rhs;

  std::vector<std::vector<typename hp::DoFHandler<dim>::active_cell_iterator> > graph;

  const unsigned int max_degree;
};



template <int dim>
class BoundaryValues : public Function<dim>
{
public:
  BoundaryValues () : Function<dim> () {}

  virtual double value (const Point<dim>   &p,
                        const unsigned int  component) const;
};


template <int dim>
double
BoundaryValues<dim>::value (const Point<dim>   &p,
                            const unsigned int  /*component*/) const
{
  double sum = 0;
  for (unsigned int d=0; d<dim; ++d)
    sum += std::sin(deal_II_numbers::PI*p[d]);
  return sum;
}


template <int dim>
class RightHandSide : public Function<dim>
{
public:
  RightHandSide () : Function<dim> () {}

  virtual double value (const Point<dim>   &p,
                        const unsigned int  component) const;
};


template <int dim>
double
RightHandSide<dim>::value (const Point<dim>   &p,
                           const unsigned int  /*component*/) const
{
  double product = 1;
  for (unsigned int d=0; d<dim; ++d)
    product *= (p[d]+1);
  return product;
}


template <int dim>
LaplaceProblem<dim>::LaplaceProblem ()
  :
  dof_handler (triangulation),
  max_degree (5)
{
  if (dim == 2)
    for (unsigned int degree=2; degree<=max_degree; ++degree)
      {
        fe_collection.push_back (FE_Q<dim>(degree));
        quadrature_collection.push_back (QGauss<dim>(degree+1));
        face_quadrature_collection.push_back (QGauss<dim-1>(degree+1));
      }
  else
    for (unsigned int degree=1; degree<max_degree-1; ++degree)
      {
        fe_collection.push_back (FE_Q<dim>(degree));
        quadrature_collection.push_back (QGauss<dim>(degree+1));
        face_quadrature_collection.push_back (QGauss<dim-1>(degree+1));
      }
}


template <int dim>
LaplaceProblem<dim>::~LaplaceProblem ()
{
  dof_handler.clear ();
}



template <int dim>
std::vector<types::global_dof_index>
LaplaceProblem<dim>::
get_conflict_indices (typename hp::DoFHandler<dim>::active_cell_iterator const &cell) const
{
  std::vector<types::global_dof_index> local_dof_indices(cell->get_fe().dofs_per_cell);
  cell->get_dof_indices(local_dof_indices);

  constraints.resolve_indices(local_dof_indices);
  return local_dof_indices;
}

template <int dim>
void LaplaceProblem<dim>::setup_system ()
{
  reference_matrix.clear();
  test_matrix.clear();
  dof_handler.distribute_dofs (fe_collection);

  solution.reinit (dof_handler.n_dofs());
  reference_rhs.reinit (dof_handler.n_dofs());
  test_rhs.reinit (dof_handler.n_dofs());

  constraints.clear ();

  DoFTools::make_hanging_node_constraints (dof_handler, constraints);

  // add boundary conditions as inhomogeneous constraints here, do it after
  // having added the hanging node constraints in order to be consistent and
  // skip dofs that are already constrained (i.e., are hanging nodes on the
  // boundary in 3D). In contrast to step-27, we choose a sine function.
  VectorTools::interpolate_boundary_values (dof_handler,
                                            0,
                                            BoundaryValues<dim>(),
                                            constraints);
  constraints.close ();

  graph = GraphColoring::make_graph_coloring(dof_handler.begin_active(),dof_handler.end(),
                                             static_cast<std_cxx1x::function<std::vector<types::global_dof_index>
                                             (typename hp::DoFHandler<dim>::active_cell_iterator const &)> >
                                             (std_cxx1x::bind(&LaplaceProblem<dim>::get_conflict_indices, this,std_cxx1x::_1)));


  BlockCompressedSimpleSparsityPattern csp (2, 2);
  for (unsigned int i=0; i<2; ++i)
    for (unsigned int j=0; j<2; ++j)
      csp.block(i,j).reinit(i==0 ? 30 : dof_handler.n_dofs() - 30,
                            j==0 ? 30 : dof_handler.n_dofs() - 30);
  csp.collect_sizes();
  DoFTools::make_sparsity_pattern (dof_handler, csp,
                                   constraints, false);
  sparsity_pattern.copy_from (csp);

  reference_matrix.reinit (sparsity_pattern);
  test_matrix.reinit (sparsity_pattern);
}



template <int dim>
void
LaplaceProblem<dim>::local_assemble (const typename hp::DoFHandler<dim>::active_cell_iterator &cell,
                                     Assembly::Scratch::Data<dim>  &scratch,
                                     Assembly::Copy::Data          &data)
{
  const unsigned int   dofs_per_cell = cell->get_fe().dofs_per_cell;

  data.local_matrix.reinit (dofs_per_cell, dofs_per_cell);
  data.local_matrix = 0;

  data.local_rhs.reinit (dofs_per_cell);
  data.local_rhs = 0;

  scratch.hp_fe_values.reinit (cell);

  const FEValues<dim> &fe_values = scratch.hp_fe_values.get_present_fe_values ();

  const RightHandSide<dim> rhs_function;

  for (unsigned int q_point=0;
       q_point<fe_values.n_quadrature_points;
       ++q_point)
    {
      const double rhs_value = rhs_function.value(fe_values.quadrature_point(q_point),0);
      for (unsigned int i=0; i<dofs_per_cell; ++i)
        {
          for (unsigned int j=0; j<dofs_per_cell; ++j)
            data.local_matrix(i,j) += (fe_values.shape_grad(i,q_point) *
                                       fe_values.shape_grad(j,q_point) *
                                       fe_values.JxW(q_point));

            data.local_rhs(i) += (fe_values.shape_value(i,q_point) *
                                  rhs_value *
                                  fe_values.JxW(q_point));
        }
    }

  data.local_dof_indices.resize (dofs_per_cell);
  cell->get_dof_indices (data.local_dof_indices);
}



template <int dim>
void
LaplaceProblem<dim>::copy_local_to_global (const Assembly::Copy::Data &data)
{
  constraints.distribute_local_to_global(data.local_matrix, data.local_rhs,
                                         data.local_dof_indices,
                                         test_matrix, test_rhs);
}



template <int dim>
void LaplaceProblem<dim>::assemble_reference ()
{
  test_matrix = 0;
  test_rhs = 0;

  Assembly::Copy::Data copy_data;
  Assembly::Scratch::Data<dim> assembly_data(fe_collection, quadrature_collection);

  for (unsigned int color=0; color<graph.size(); ++color)
    for (typename std::vector<typename hp::DoFHandler<dim>::active_cell_iterator>::const_iterator p = graph[color].begin();
         p != graph[color].end(); ++p)
      {
        local_assemble(*p, assembly_data, copy_data);
        copy_local_to_global(copy_data);
      }

  reference_matrix.add(1., test_matrix);
  reference_rhs = test_rhs;
}



template <int dim>
void LaplaceProblem<dim>::assemble_test ()
{
  test_matrix = 0;
  test_rhs = 0;

  WorkStream::
    run (graph,
         std_cxx1x::bind (&LaplaceProblem<dim>::
                          local_assemble,
                          this,
                          std_cxx1x::_1,
                          std_cxx1x::_2,
                          std_cxx1x::_3),
         std_cxx1x::bind (&LaplaceProblem<dim>::
                          copy_local_to_global,
                          this,
                          std_cxx1x::_1),
         Assembly::Scratch::Data<dim>(fe_collection, quadrature_collection),
         Assembly::Copy::Data ());

  test_matrix.add(-1, reference_matrix);

  double frobenius_norm = 0;
  for (unsigned int i=0; i<2; ++i)
    for (unsigned int j=0; j<2; ++j)
      frobenius_norm += test_matrix.block(i,j).frobenius_norm();

  deallog << "log error in matrix norm: " << std::log(frobenius_norm) << std::endl;
  test_rhs.add(-1., reference_rhs);
  deallog << "log error in vector norm: " << std::log(test_rhs.l2_norm()) << std::endl;
}



template <int dim>
void LaplaceProblem<dim>::postprocess ()
{
  Vector<float> estimated_error_per_cell (triangulation.n_active_cells());
  for (unsigned int i=0; i<estimated_error_per_cell.size(); ++i)
    estimated_error_per_cell(i) = i;

  GridRefinement::refine_and_coarsen_fixed_number (triangulation,
                                                   estimated_error_per_cell,
                                                   0.3, 0.03);
  triangulation.execute_coarsening_and_refinement ();

  for (typename hp::DoFHandler<dim>::active_cell_iterator cell = dof_handler.begin_active();
       cell != dof_handler.end(); ++cell)
    cell->set_active_fe_index (rand() % fe_collection.size());
}




template <int dim>
void LaplaceProblem<dim>::run ()
{
  for (unsigned int cycle=0; cycle<3; ++cycle)
    {
      if (cycle == 0)
        {
          GridGenerator::hyper_shell(triangulation,
                                     Point<dim>(),
                                     0.5, 1., (dim==3) ? 96 : 12, false);
        }

      setup_system ();

      assemble_reference ();
      assemble_test ();

      if (cycle < 2)
        postprocess ();
    }
}



int main ()
{
  deallog << std::setprecision (2);
  logfile << std::setprecision (2);
  deallog.attach(logfile);
  deallog.depth_console(0);
  deallog.threshold_double(1.e-8);

  {
    deallog.push("2d");
    LaplaceProblem<2> laplace_problem;
    laplace_problem.run ();
    deallog.pop();
  }

  {
    deallog.push("3d");
    LaplaceProblem<3> laplace_problem;
    laplace_problem.run ();
    deallog.pop();
  }
}
