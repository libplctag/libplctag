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
