/*
 * Copyright 2017-2018, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *     * Neither the name of the copyright holder nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * vec.h -- vector interface
 */

#ifndef PMDK_VEC_H
#define PMDK_VEC_H 1

#include "valgrind_internal.h"

#define VEC_INIT_SIZE (64)

#define VEC(name, type)\
struct name {\
	type *buffer;\
	size_t size;\
	size_t capacity;\
}

#define VEC_INITIALIZER {NULL, 0, 0}

#define VEC_INIT(vec) do {\
	(vec)->buffer = NULL;\
	(vec)->size = 0;\
	(vec)->capacity = 0;\
} while (0)

#define VEC_REINIT(vec) do {\
	VALGRIND_ANNOTATE_NEW_MEMORY((vec), sizeof(*vec));\
	VALGRIND_ANNOTATE_NEW_MEMORY((vec)->buffer,\
		(sizeof(*(vec)->buffer) * ((vec)->capacity)));\
	(vec)->size = 0;\
} while (0)

#define VEC_RESERVE(vec, ncapacity) do {\
	size_t ncap = (ncapacity) == 0 ? VEC_INIT_SIZE : (ncapacity);\
	if (ncap > (vec)->size) {\
		void *tbuf = Realloc((vec)->buffer,\
			sizeof(*(vec)->buffer) * ncap);\
		ASSERTne(tbuf, NULL);\
		/* there's no way to return a value from a macro in MSVC... */\
		(vec)->buffer = tbuf;\
		(vec)->capacity = ncap;\
	}\
} while (0)

#define VEC_POP_BACK(vec) do {\
	(vec)->size -= 1;\
} while (0)

#define VEC_FRONT(vec)\
(vec)->buffer[0]

#define VEC_BACK(vec)\
(vec)->buffer[(vec)->size - 1]

#define VEC_ERASE_BY_POS(vec, pos) do {\
	if ((pos) != ((vec)->size - 1))\
		(vec)->buffer[(pos)] = VEC_BACK(vec);\
	VEC_POP_BACK(vec);\
} while (0)

#define VEC_ERASE_BY_PTR(vec, element) do {\
	if ((element) != &VEC_BACK(vec))\
		*(element) = VEC_BACK(vec);\
	VEC_POP_BACK(vec);\
} while (0)

#define VEC_INC_BACK(vec) do {\
	if ((vec)->capacity == (vec)->size)\
		VEC_RESERVE((vec), ((vec)->capacity * 2));\
	(vec)->size++;\
} while (0)

#define VEC_PUSH_BACK(vec, element) do {\
	VEC_INC_BACK(vec);\
	(vec)->buffer[(vec)->size - 1] = (element);\
} while (0)

#define VEC_FOREACH(el, vec)\
for (size_t _vec_i = 0;\
	_vec_i < (vec)->size && ((el = (vec)->buffer[_vec_i]), 1);\
	++_vec_i)

#define VEC_FOREACH_BY_POS(elpos, vec)\
for (elpos = 0; elpos < (vec)->size; ++elpos)

#define VEC_FOREACH_BY_PTR(el, vec)\
for (size_t _vec_i = 0;\
	_vec_i < (vec)->size && ((el = &(vec)->buffer[_vec_i]), 1);\
	++_vec_i)

#define VEC_SIZE(vec)\
((vec)->size)

#define VEC_CAPACITY(vec)\
((vec)->capacity)

#define VEC_ARR(vec)\
((vec)->buffer)

#define VEC_GET(vec, id)\
(&(vec)->buffer[id])

#define VEC_CLEAR(vec) do {\
	(vec)->size = 0;\
} while (0)

#define VEC_DELETE(vec) do {\
	Free((vec)->buffer);\
} while (0)

#endif /* PMDK_VEC_H */
