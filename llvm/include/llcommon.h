/**
 * This file is part of DBrew, the dynamic binary rewriting library.
 *
 * (c) 2015-2016, Josef Weidendorfer <josef.weidendorfer@gmx.de>
 *
 * DBrew is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License (LGPL)
 * as published by the Free Software Foundation, either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * DBrew is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with DBrew.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * \file
 **/

#ifndef LL_COMMON_PUBLIC_H
#define LL_COMMON_PUBLIC_H

#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>


struct LLVMFunctionDecl;

typedef struct LLFunctionDecl LLFunctionDecl;

struct LLFunction;

typedef struct LLFunction LLFunction;

struct LLState;

typedef struct LLState LLState;

struct LLConfig {
    /**
     * \brief The name of the function
     **/
    const char* name;

    size_t stackSize;

    // FunctionSignature sign;
    size_t noaliasParams;

    bool fixFirstParam;
    uintptr_t firstParam;
    size_t firstParamLength;
};

typedef struct LLConfig LLConfig;

#endif