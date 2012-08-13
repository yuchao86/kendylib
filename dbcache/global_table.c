#include "global_table.h"
#include <stdlib.h>
#include <stdio.h>
#include "kstring.h"


enum
{
	_EMPTY = 0,
	_DELETE,
	_USED,
};

struct tb_item
{
	uint64_t hash_code;
	string_t key;
	db_element_t val;
	struct tb_item *next;
	struct tb_item *pre;
	int8_t flag;
};

struct global_table
{
	int64_t slot_size;
	int64_t size;
	int64_t expand_size;
	struct tb_item *data;
	struct tb_item head;
	struct tb_item tail;
	struct tb_item *last_shrink_node;
};

static inline int32_t _hash_key_eq_(string_t l,string_t r)
{
	return string_compare(l,r);
}



global_table_t global_table_create(int32_t initsize)
{
	if(initsize > 0)
	{
		global_table_t gt = calloc(1,sizeof(*gt));
		gt->slot_size = initsize;
		gt->expand_size = gt->slot_size - gt->slot_size/4;
		gt->data = calloc(initsize,sizeof(struct tb_item));
		gt->head.pre = gt->head.next = &gt->tail;
		gt->tail.pre = gt->tail.next = &gt->head;
		gt->last_shrink_node = gt->head.next;
		return gt;
	}
	return NULL;

}

static inline int8_t isempty(struct tb_item *item)
{
	if(item->flag == _EMPTY || item->flag == _DELETE)
		return 1;
	if(item->val->type == DB_ARRAY && ((db_array_t)item->val)->data == NULL)
	{
		db_array_release((db_array_t*)&(item->val));
		return 1;
	}
	return 0;
}

static inline struct tb_item* _hash_map_insert(global_table_t h,string_t key,db_element_t val,uint64_t hash_code)
{
	int64_t slot = hash_code % h->slot_size;
	int64_t check_count = 0;
	struct tb_item *item = 0;
	while(check_count < h->slot_size)
	{
		item = &(h->data[slot]);
		if(isempty(item))
		{
			item->flag = _USED;
			item->hash_code = hash_code;
			item->key = key;
			item->val = val;
			item->pre = h->tail.pre;
			h->tail.pre->next = item;
			item->next = &(h->tail);
			h->tail.pre = item;
			++h->size;
			return item;
		}
		else
			if(hash_code == item->hash_code &&  _hash_key_eq_(key,item->key) == 0)
				return item;
		slot = (slot + 1)%h->slot_size;
		check_count++;
	}
	return NULL;
}

static inline int32_t _hash_map_expand(global_table_t h)
{
	uint32_t old_slot_size = h->slot_size;
	struct tb_item *old_items = h->data;
	uint32_t i = 0;
	h->slot_size <<= 1;
	h->data = calloc(h->slot_size,sizeof(struct tb_item));
	if(!h->data)
	{
		h->data = old_items;
		h->slot_size >>= 1;
		return -1;
	}
	h->head.pre = h->head.next = &(h->tail);
	h->tail.pre = h->tail.next = &(h->head);	
	for(; i < old_slot_size; ++i)
	{
		struct tb_item *_item = &old_items[i];
		if(_item->flag == _USED)
			_hash_map_insert(h,_item->key,_item->val,_item->hash_code);
	}
	h->expand_size = h->slot_size - h->slot_size/4;
	free(old_items);
	//h->last_shrink_node = h->head.next;
	return 0;
}

static inline struct tb_item* _hash_map_find(global_table_t h,string_t key,uint64_t hash_code)
{
	if(hash_code == 0)
		hash_code = global_hash(string_c_str(key));
	int64_t  slot = hash_code % h->slot_size;
	int64_t  check_count = 0;
	struct tb_item *item = 0;
	while(check_count < h->slot_size)
	{
		item = &h->data[slot];
		if(item->flag == _EMPTY)
			return NULL;
		if(item->hash_code == hash_code && _hash_key_eq_(key,item->key) == 0)
		{
			if(item->flag == _DELETE)
				return NULL;
			else
				return item;
		}
		slot = (slot + 1)%h->slot_size;
		check_count++;
	}
	return NULL;
}

static inline void global_table_raw_set(global_table_t gt,struct tb_item *item,db_element_t e)
{
	if(e == NULL)
	{
		//remove
		string_destroy(&(item->key));		
		gt->size -= item->val->hash_count;
		if(gt->last_shrink_node == item)
			gt->last_shrink_node = item->next;		
		item->pre->next = item->next;
		item->next->pre = item->pre;
		item->next = item->pre = NULL;
		
		if(item->val->type == DB_ARRAY)
			db_array_clear((db_array_t)item->val);
		db_element_release(&(item->val));
		item->val = NULL;
		item->flag = _DELETE;
	}
	else
	{
		db_element_t old = item->val;
		if(old)
			db_element_release(&old);
		e = db_element_acquire(NULL,e);
		item->val = e;
		++e->hash_count;
	}
}

db_element_t global_table_add(global_table_t gt,const char *key,db_element_t e,uint64_t hash_code)
{
	if(!key || !e)
		return NULL;
		
	db_element_t ret;
	//if not enough space,expand first
	if(gt->slot_size < 0x80000000 && gt->size >= gt->expand_size)
		_hash_map_expand(gt);
	if(gt->size >= gt->slot_size)
		return NULL;	
		
	string_t _key = string_create(key);	
	struct tb_item *item = _hash_map_insert(gt,_key,e,hash_code);
	if(!item)
	{
		//not enough space
		string_destroy(&_key);
		return NULL;
	}
	
	if(item->val != e)
	{
		string_destroy(&_key);
		if(e->type == DB_LIST)
			return NULL;
		db_element_t _e = item->val;
		db_list_t dbl;
		if(_e->type == DB_ARRAY)
		{
			dbl = db_list_create();
			db_list_append(dbl,(db_array_t)_e);
			global_table_raw_set(gt,item,(db_element_t)dbl);
			db_list_append(dbl,(db_array_t)e);
			ret = (db_element_t)dbl;
			db_list_release(&dbl);
			--_e->hash_count;
		}
		else
		{
			dbl = (db_list_t)_e;
			db_list_append(dbl,(db_array_t)e);
			ret = (db_element_t)dbl;
		}
	}
	else
	{
		e = db_element_acquire(NULL,e);
		ret = (db_element_t)e;
	}
	if(ret->type == DB_LIST && ret->hash_count == 1)
	{
		if(gt->last_shrink_node == &gt->tail)
			gt->last_shrink_node == item;
	}
	return ret;
}

db_element_t global_table_find(global_table_t gt,const char *key,uint64_t hash_code)
{
	string_t _key = string_create(key);
	struct tb_item *item = _hash_map_find(gt,_key,hash_code);
	string_destroy(&_key);
	if(item)
		return item->val;
	return NULL;
}

int32_t global_table_remove(global_table_t gt,const char *key,uint64_t hash_code)
{
	
	string_t _key = string_create(key);
	struct tb_item *item = _hash_map_find(gt,_key,hash_code);
	string_destroy(&_key);
	if(item)
	{		
		if(item->val->type == DB_LIST)
		{
		  db_list_t l = (db_list_t)item->val;
		  list_node *head = link_list_head(l->l);
		  while(head)
		  {
			  db_array_t a = (db_array_t)((struct db_node*)head)->array; 
			  head = head->next;
			  db_array_clear(a);
			  db_array_release(&a);
			  gt->size -= a->base.hash_count;
		  }		  			
		}
 
		global_table_raw_set(gt,item,NULL);
		return 0;		
	}
	return -1;
}

void global_table_destroy(global_table_t *gt)
{
	int64_t i = 0;
	int64_t size = (*gt)->size;
	struct tb_item *item = (*gt)->head.next;
	for( ; i < size; ++i)
	{
		struct tb_item *tmp = item;
		item = item->next;
		global_table_raw_set(*gt,tmp,NULL);
	}
	free((*gt)->data);
	free(*gt);
	*gt = NULL;
}
#include "SysTime.h"
void global_table_shrink(global_table_t gt,uint32_t maxtime)
{
	if(maxtime == 0)
		return;
	uint32_t tick =GetCurrentMs();
	uint32_t end_tick = tick + maxtime;
	int8_t finish = 1;
	while(gt->last_shrink_node != &(gt->tail))
	{
		if(gt->last_shrink_node->val->type == DB_LIST)
			//do shrink
			finish = db_list_shrink((db_list_t)gt->last_shrink_node->val,end_tick-tick);
		tick = GetCurrentMs();
		if(tick >= end_tick)
			break;
	} 
	if(finish == 1 && gt->last_shrink_node == &gt->tail)
		gt->last_shrink_node == gt->head.next;
}
