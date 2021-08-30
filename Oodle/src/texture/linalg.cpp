// Copyright Epic Games, Inc. All Rights Reserved.
// This source file is licensed solely to users who have
// accepted a valid Unreal Engine license agreement 
// (see e.g., https://www.unrealengine.com/eula), and use
// of this source file is governed by such agreement.

#include "linalg.h"
#include <math.h>

OODLE_NS_START

bool LinAlg_CholeskyUTU(F32 * matrix, int dim)
{
	// The factorization we want is (for an example 3x3 matrix):
	//
	//   U^T U = A (where A symmetric)
	//
	// writing it out: (and omitting the symmetric parts of A
	// that are assumed to not be stored in the source)
	//
	//   |u11        | |u11 u12 u13|   |a11 a12 a13|
	//   |u12 u22    | |    u22 u23| = |    a22 a23|
	//   |u13 u23 u33| |        u33|   |(sym)   a33|
	//
	// We implement a row-wise factorization variant.
	//
	// Multiplying out yields: (first row of results)
	//
	//   u11 * u11 = a11  => u11 = sqrt(a11)
	//   u11 * u12 = a12  => u12 = a12 / u11
	//   u11 * u13 = a13  => u13 = a13 / u11
	//
	// Second row of results:
	//
	//   u12 * u12 + u22 * u22 = a22  => u22 = sqrt(a22 - u12*u12)
	//   u12 * u13 + u22 * u23 = a23  => u23 = (a23 - u12*u13) / u22
	//
	// which allows us to deduce the formulas used below.

	for (int i = 0; i < dim; i++)
	{
		// Diagonal entry:
		// Take diagonal entry a_ii, and subtract the squares of
		// all elements in the column above a_ii.
		F64 diag = matrix[i*dim + i];
		for (int j = 0; j < i; j++)
			diag -= (F64)matrix[j*dim + i] * (F64)matrix[j*dim + i];

		// If this is <=0, the matrix is not positive definite; bail.
		if (diag <= 0.0)
			return false;

		// Matrix is positive definite, take the square root and continue.
		diag = sqrt(diag);
		matrix[i*dim + i] = (F32)diag;

		// Rest of the row
		for (int j = i + 1; j < dim; j++)
		{
			// Take a_ij, subtract the dot product of the parts of
			// columns i and j above row i, and then divide by the
			// just-computed diagonal element for row i.
			F64 sum = matrix[i*dim + j];
			for (int k = 0; k < i; k++)
				sum -= (F64)matrix[k*dim + i] * (F64)matrix[k*dim + j];

			matrix[i*dim + j] = (F32)(sum / diag);
		}
	}

	return true;
}

void LinAlg_CholeskySolve(const F32 * matrix, int dim, F32 * b)
{
	// b = U^T U x = U^T (U x)
	// therefore, let
	//   U x = c
	// then first solve
	//   U^T c = b (forward substitution)
	// for c, then
	//   U x = c (backward substitution)
	// for x.
	
	// Forward substitution: U^T c = b.
	// Writing it out:
	//
	//   |u11        | |c1|   |b1|
	//   |u12 u22    | |c2| = |b2|
	//   |u13 u23 u33| |c3|   |b3|
	//
	// so
	//
	//   u11 c1 = b1                   => c1 = b1 / u11
	//   u12 c1 + u22 c2 = b2          => c2 = (b2 - u12 c1) / u22
	//   u13 c1 + u23 c2 + u33 c3 = b3 => c3 = (b3 - u13 c1 - u23 c2) / u33
	//
	// and so forth.
	for (int i = 0; i < dim; i++)
	{
		F64 sum = b[i];
		for (int j = 0; j < i; j++)
			sum -= (F64)b[j] * (F64)matrix[j*dim + i];

		b[i] = (F32)(sum / matrix[i*dim + i]);
	}

	// Backward substitution: U x = c.
	//
	// Again, written out:
	// 
	//   |u11 u12 u13| |x1| = |c1|
	//   |    u22 u23| |x2| = |c2|
	//   |        u33| |x3| = |c3|
	//
	// so
	//
	//   u33 x3 = c3                   => x3 = c3 / u33
	//   u22 x2 + u23 x3 = c2          => x2 = (c2 - u23 x3) / u22
	//   u11 x1 + u12 x2 + u13 x3 = c1 => x1 = (c1 - u12 x2 - u13 x3) / u11
	for (int i = dim - 1; i >= 0; i--)
	{
		F64 sum = b[i];
		for (int j = i + 1; j < dim; j++)
			sum -= (F64)b[j] * (F64)matrix[i*dim + j];

		b[i] = (F32)(sum / matrix[i*dim + i]);
	}
}

OODLE_NS_END

