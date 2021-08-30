// Copyright Epic Games, Inc. All Rights Reserved.
// This source file is licensed solely to users who have
// accepted a valid Unreal Engine license agreement 
// (see e.g., https://www.unrealengine.com/eula), and use
// of this source file is governed by such agreement.

#pragma once

#include "oodlebase.h"

OODLE_NS_START

// NOTE: as of this writing (Jun 2019), everything in here
// is intended for fairly small matrices (maybe 20x20 max).
//
// No attempt is made to use vectorization, multiple threads
// or cache blocking.

// ---- General Linear Algebra routines

// Computes upper-triangular Cholesky decomposition
//
//   A = U^T U
//
// of a row-major symmetric dim*dim matrix A (only upper
// triangle is accessed). Updates A in place to contain
// the factorization.
//
// Returns true on success, false if the matrix is not
// positive definite. In the latter case A has been
// overwritten with a partial factorization (which is,
// for all practical purposes, garbage.)
bool LinAlg_CholeskyUTU(F32 * A, int dim);

// Solves
//
//    U^T U x = b
//
// overwriting b with x (in place).
//
// where U is the upper-right triangle of the given row-major matrix.
// This is the factorization returned by LinAlg_CholeskyUTU.
// The remaining elements of matrix are ignore.
void LinAlg_CholeskySolve(const F32 * matrix, int dim, F32 * b);

OODLE_NS_END

