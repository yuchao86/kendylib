/*	
    Copyright (C) <2012>  <huangweilook@21cn.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef _GLOBAL_TABLE_H
#define _GLOBAL_TABLE_H
#include "dbtype.h"
#include "data_struct.h"
#include "common_hash_function.h"
#include <string.h>

typedef struct global_table *global_table_t;

global_table_t global_table_create(int32_t initsize);
void           global_table_destroy(global_table_t*);


db_element_t   global_table_find(global_table_t,const char *key,uint64_t hash_code);
int32_t        global_table_remove(global_table_t,const char *key,uint64_t hash_code);
db_element_t   global_table_add(global_table_t,const char *key,db_element_t e,uint64_t hash_code);

static inline  uint64_t global_hash(const char *key)
{
	return burtle_hash((uint8_t*)key,strlen(key),1);
}

//collect unused db_element_t and destroy it
void           global_table_shrink(global_table_t,uint32_t maxtime);

#endif
