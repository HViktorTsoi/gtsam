/* ----------------------------------------------------------------------------

 * GTSAM Copyright 2010, Georgia Tech Research Corporation,
 * Atlanta, Georgia 30332-0415
 * All Rights Reserved
 * Authors: Frank Dellaert, et al. (see THANKS for the full author list)

 * See LICENSE for the license information

 * -------------------------------------------------------------------------- */

/**
 * @file CuSparseSolver.cpp
 *
 * @brief SuiteSparse based linear solver backend for GTSAM
 *
 * @date Jun 2020
 * @author Fan Jiang
 */

#include "gtsam/linear/CuSparseSolver.h"

#ifdef GTSAM_USE_CUSPARSE
#include <cusolverSp.h>
#include <cuda_runtime.h>
#include <cusolverSp.h>

#endif

#include "gtsam/linear/SparseEigenSolver.h"

namespace gtsam {
  CuSparseSolver::CuSparseSolver(CuSparseSolver::CuSparseSolverType type,
                                              const Ordering &ordering) {
    solverType = type;
    this->ordering = ordering;
  }

  bool CuSparseSolver::isIterative() {
    return false;
  }

  bool CuSparseSolver::isSequential() {
    return false;
  }

#ifdef GTSAM_USE_CUSPARSE

#define S1(x) #x
#define S2(x) S1(x)
#define ____LOCATION __FILE__ " : " S2(__LINE__)

  void checkCUDAError(cudaError code, const char* location) {
    if(code != cudaSuccess) {
      throw std::runtime_error(std::string("cudaMalloc error ") + std::to_string(code) + std::string(" at ") + std::string(location));
    }
  }

#define CHECK_CUDA_ERROR(code) checkCUDAError(code, ____LOCATION);

  void checkCuSolverError(cusolverStatus_t code, const char* location) {
    if(code != CUSOLVER_STATUS_SUCCESS) {
      throw std::runtime_error(std::string("cuSolver error ") + std::to_string(code) + std::string(" at ") + std::string(location));
    }
  }

#define CHECK_CUSOLVER_ERROR(code) checkCuSolverError(code, ____LOCATION);

  void checkCuSparseError(cusparseStatus_t code, const char* location) {
    if(code != CUSPARSE_STATUS_SUCCESS) {
      throw std::runtime_error(std::string("cuSparse error ") + std::to_string(code) + std::string(" at ") + std::string(location));
    }
  }

#define CHECK_CUSPARSE_ERROR(code) checkCuSparseError(code, ____LOCATION);

  void EigenSparseToCuSparseTranspose(
      const Eigen::SparseMatrix<double> &mat, int **row, int **col, double **val)
  {
    const int num_non0  = mat.nonZeros();
    const int num_outer = mat.cols() + 1;

    cudaError err;
    err = cudaMalloc(reinterpret_cast<void **>(row), sizeof(int) * num_outer);
    if(err != cudaSuccess) {
      throw std::runtime_error(std::string("cudaMalloc error: out of memory? trying to allocate ") + std::to_string(err));
    }
    if(cudaMalloc(reinterpret_cast<void **>(col), sizeof(int) * num_non0) != cudaSuccess) {
      cudaFree(row);
      throw std::runtime_error("cudaMalloc error: out of memory?");
    }
    if(cudaMalloc(reinterpret_cast<void **>(val), sizeof(double) * num_non0) != cudaSuccess) {
      cudaFree(row);
      cudaFree(col);
      throw std::runtime_error("cudaMalloc error: out of memory?");
    }

    CHECK_CUDA_ERROR(cudaMemcpy(
        *val, mat.valuePtr(),
        num_non0 * sizeof(double),
        cudaMemcpyHostToDevice));
    CHECK_CUDA_ERROR(cudaMemcpy(*row, mat.outerIndexPtr(),
        num_outer * sizeof(int),
        cudaMemcpyHostToDevice));
    CHECK_CUDA_ERROR(cudaMemcpy(
        *col, mat.innerIndexPtr(),
        num_non0 * sizeof(int),
        cudaMemcpyHostToDevice));
  }

  VectorValues CuSparseSolver::solve(const gtsam::GaussianFactorGraph &gfg) {
    if (solverType == QR) {
      throw std::invalid_argument("This solver does not support QR.");
    } else if (solverType == CHOLESKY) {
      gttic_(CuSparseSolver_optimizeEigenCholesky);
      Eigen::SparseMatrix<double>
          Ab = SparseEigenSolver::sparseJacobianEigen(gfg, ordering);
      auto rows = Ab.rows(), cols = Ab.cols();
      Eigen::SparseMatrix<double> A = Ab.block(0, 0, rows, cols - 1);
      auto At = A.transpose();
      Matrix b = At * Ab.col(cols - 1);

      Eigen::SparseMatrix<double>
          AtA(A.cols(), A.cols());
      AtA.selfadjointView<Eigen::Upper>().rankUpdate(At);
      AtA.makeCompressed();

      gttic_(CuSparseSolver_optimizeEigenCholesky_solve);

      cusolverSpHandle_t solverHandle;
      CHECK_CUSOLVER_ERROR(cusolverSpCreate(&solverHandle));

      cusparseMatDescr_t descrA;
      CHECK_CUSPARSE_ERROR(cusparseCreateMatDescr(&descrA));
      CHECK_CUSPARSE_ERROR(cusparseSetMatType(descrA, CUSPARSE_MATRIX_TYPE_GENERAL));

      int *AtA_row(NULL), *AtA_col(NULL);
      double *AtA_val(NULL);

      EigenSparseToCuSparseTranspose(AtA, &AtA_row, &AtA_col, &AtA_val);

      double *x_gpu(NULL), *b_gpu(NULL);

      CHECK_CUDA_ERROR(cudaMalloc(&x_gpu, sizeof(double) * AtA.cols()));
      CHECK_CUDA_ERROR(cudaMalloc(&b_gpu, sizeof(double) * AtA.cols()));

      CHECK_CUDA_ERROR(cudaMemcpy(b_gpu, b.data(),
                 sizeof(double) * AtA.cols(),
                 cudaMemcpyHostToDevice));

      int singularity = 0;
      const double tol = 0.00001;

      std::cout << "AtA_row: " << AtA_row << ", AtA_col: " << AtA_col << std::endl;
      std::cout << "AtA.row: " << AtA.rows() << ", AtA.col: " << AtA.cols() << std::endl;

      // no internal reordering, so only lower part (upper part of CSC) is used
      CHECK_CUSOLVER_ERROR(cusolverSpDcsrlsvchol(
          solverHandle, AtA.rows(), AtA.nonZeros(), descrA,
          AtA_val, AtA_row, AtA_col, b_gpu, tol, 0, x_gpu, &singularity));

      Vector x;
      x.resize(A.cols());
      CHECK_CUDA_ERROR(cudaMemcpy(x.data(), x_gpu, sizeof(double) * A.cols(),
                                 cudaMemcpyDeviceToHost));

      cudaFree(AtA_val);
      cudaFree(AtA_row);
      cudaFree(AtA_col);
      cudaFree(b_gpu);
      cudaFree(x_gpu);

      if (singularity != -1)
        throw std::runtime_error(std::string("ILS in CUDA Solver, singularity: ") + std::to_string(singularity));

      gttoc_(CuSparseSolver_optimizeEigenCholesky_solve);

      // NOTE: b is reordered now, so we need to transform back the order.
      // First find dimensions of each variable
      std::map<Key, size_t> dims;
      for (const boost::shared_ptr<GaussianFactor> &factor : gfg) {
        if (!static_cast<bool>(factor))
          continue;

        for (auto it = factor->begin(); it != factor->end(); ++it) {
          dims[*it] = factor->getDim(it);
        }
      }

      VectorValues vv;

      std::map<Key, size_t> columnIndices;

      {
        size_t currentColIndex = 0;
        for (const auto key : ordering) {
          columnIndices[key] = currentColIndex;
          currentColIndex += dims[key];
        }
      }

      for (const std::pair<const Key, unsigned long> keyDim : dims) {
        vv.insert(keyDim.first, x.segment(columnIndices[keyDim.first], keyDim.second));
      }

      return vv;
    }

    throw std::exception();
  }
#else
  VectorValues CuSparseSolver::solve(const gtsam::GaussianFactorGraph &gfg) {
    throw std::invalid_argument("This GTSAM is compiled without Cholmod support");
  }
#endif
}
