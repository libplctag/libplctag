/***************************************************************************
 *   Copyright (C) 2017 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Library/Lesser General Public License as*
 *   published by the Free Software Foundation; either version 2 of the    *
 *   License, or (at your option) any later version.                       *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#pragma once


typedef struct vector_t *vector_p;

extern vector_p vector_create(int capacity, int max_inc);
extern int vector_length(vector_p vec);
extern int vector_put(vector_p vec, int index, void * ref);
extern void *vector_get(vector_p vec, int index);
extern int vector_on_each(vector_p vec, int (*callback_func)(vector_p vec, int index, void **data, int arg_count, void **args), int num_args, ...);
extern void *vector_remove(vector_p vec, int index);
extern int vector_destroy(vector_p vec);

