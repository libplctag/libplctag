/***************************************************************************
 *   Copyright (C) 2015 by OmanTek                                         *
 *   Author Kyle Hayes  kylehayes@omantek.com                              *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Library General Public License as       *
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

/*
 * attr.h
 *
 *  Created on: Nov 27, 2012
 *      Author: Kyle Hayes
 */

#ifndef ATTR_H_
#define ATTR_H_



typedef struct attr_t *attr;

extern attr attr_create(void);
extern attr attr_create_from_str(const char *attr_str);
extern int attr_set_str(attr attrs, const char *name, const char *val);
extern int attr_set_int(attr attrs, const char *name, int val);
extern int attr_set_float(attr attrs, const char *name, float val);
extern const char *attr_get_str(attr attrs, const char *name, const char *def);
extern int attr_get_int(attr attrs, const char *name, int def);
extern float attr_get_float(attr attrs, const char *name, float def);
extern int att_remove(attr attrs, const char *name);
extern void attr_destroy(attr attrs);


#endif /* ATTR_H_ */
