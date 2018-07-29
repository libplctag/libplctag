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

#ifndef __UTIL_HASHTABLE_H__
#define __UTIL_HASHTABLE_H__ 1


typedef struct hashtable_t *hashtable_p;

extern hashtable_p hashtable_create(int size);
extern void *hashtable_get(hashtable_p table, void *key, int key_len);
extern int hashtable_put(hashtable_p table, void *key, int key_len, void *arg);
extern int hashtable_on_each(hashtable_p table, int (*callback_func)(hashtable_p table, void *key, int key_len, void *data));
extern void *hashtable_remove(hashtable_p table, void *key, int key_len);
extern int hashtable_destroy(hashtable_p table);


#endif
