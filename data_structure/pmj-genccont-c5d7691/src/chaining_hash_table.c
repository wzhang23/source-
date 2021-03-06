/*
Copyright (c) 2011 Phil Jordan <phil@philjordan.eu>

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

   1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.

   2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.

   3. This notice may not be removed or altered from any source
   distribution.
*/

#include "chaining_hash_table.h"

#ifdef __GNUC__
#define GENC_UNUSED __attribute__((unused))
#else
#define GENC_UNUSED
#endif

#ifndef GENC_MEMSET
#if !defined(KERNEL) && !defined(__KERNEL__)
/* required for memset() - if no memset is available or you'd like to provide
 * your own, compile with GENC_MEMSET defined to the name of your implementation. */
/*#include <stdio.h>*/
#include <string.h>
#else
void* GENC_MEMSET(void *, int, size_t);
#endif

#define GENC_MEMSET memset
#endif

#ifdef __cplusplus
#define GENC_CXX_CAST(TYPE, EXPR) static_cast<TYPE>(EXPR)
#else
#define GENC_CXX_CAST(TYPE, EXPR) (EXPR)
#endif


typedef struct slist_head slist_head_t;
typedef struct genc_chaining_hash_table genc_chaining_hash_table_t;

/* integer hashes described in http://www.cris.com/~Ttwang/tech/inthash.htm */
size_t genc_hash_uint32(uint32_t key)
{
  key = ~key + (key << 15);
  key = key ^ (key >> 12);
  key = key + (key << 2);
  key = key ^ (key >> 4);
  key = key * 2057;
  key = key ^ (key >> 16);
  return key;
}

#ifdef __LP64__
/* 64 bit -> 64 bit hash */
size_t genc_hash_uint64(uint64_t key)
{
  key = (~key) + (key << 21);
  key = key ^ (key >> 24);
  key = (key + (key << 3)) + (key << 8);
  key = key ^ (key >> 14);
  key = (key + (key << 2)) + (key << 4);
  key = key ^ (key >> 28);
  key = key + (key << 31);
  return key;
}
#else
size_t genc_hash_uint64(uint64_t key)
{
  key = (~key) + (key << 18);
  key = key ^ (key >> 31);
  key = key * 21;
  key = key ^ (key >> 11);
  key = key + (key << 6);
  key = key ^ (key >> 22);
  return (size_t)key;
}
#endif


size_t genc_uint32_key_hash(void* item, void* opaque_unused GENC_UNUSED)
{
	return genc_hash_uint32(*(uint32_t*)item);
}
size_t genc_uint64_key_hash(void* item, void* opaque_unused GENC_UNUSED)
{
	return genc_hash_uint64(*(uint64_t*)item);
}
int genc_uint64_keys_equal(void* id1, void* id2, void* opaque_unused GENC_UNUSED)
{
	return *(uint64_t*)id1 == *(uint64_t*)id2;
}



/* Initialises the empty hash table with the given function implementations and capacity.
 * Defaults (70, 0) are used for load factor percentage thresholds for growing and shrinking. */
int genc_chaining_hash_table_init(
	struct genc_chaining_hash_table* table,
	genc_chaining_key_hash_fn hash_fn,
	genc_chaining_hash_get_item_key_fn get_key_fn,
	genc_chaining_hash_key_equality_fn key_equality_fn,
	genc_realloc_fn realloc_fn,
	void* opaque,
	size_t initial_capacity_pow2)
{
	return genc_chaining_hash_table_init_ext(table, hash_fn, get_key_fn, key_equality_fn, realloc_fn, opaque, initial_capacity_pow2, 70, 0);
}

static GENC_INLINE int genc_is_pow2(size_t val)
{
	return val != 0ul && 0ul == (val & (val - 1ul));
}


/* initialises the hash table with non-default thresholds. Table will only grow on insertion and shrink on removal. */
int genc_chaining_hash_table_init_ext(
	struct genc_chaining_hash_table* table,
	genc_chaining_key_hash_fn hash_fn,
	genc_chaining_hash_get_item_key_fn get_key_fn,
	genc_chaining_hash_key_equality_fn key_equality_fn,
	genc_realloc_fn realloc_fn,
	void* opaque,
	size_t initial_capacity_pow2,
	uint8_t load_percent_grow_threshold,
	uint8_t load_percent_shrink_threshold)
{
	void* buckets;
	if (!genc_is_pow2(initial_capacity_pow2))
		return 0;
	buckets = realloc_fn(NULL, 0, initial_capacity_pow2 * sizeof(slist_head_t*));
	if (!buckets)
		return 0;
	GENC_MEMSET(buckets, 0, initial_capacity_pow2 * sizeof(slist_head_t*));
	
	table->hash_fn = hash_fn;
	table->get_key_fn = get_key_fn;
	table->key_equality_fn = key_equality_fn;
	table->realloc_fn = realloc_fn;
	table->opaque = opaque;
	table->capacity = initial_capacity_pow2;
	table->item_count = 0;
	table->buckets = GENC_CXX_CAST(slist_head_t**, buckets);
	table->load_percent_grow_threshold = load_percent_grow_threshold;
	table->load_percent_shrink_threshold = load_percent_shrink_threshold;
	
	return 1;
}

/* Returns the current number of items in the hash table. */
size_t genc_cht_count(struct genc_chaining_hash_table* table)
{
	return table->item_count;
}

size_t genc_cht_capacity(struct genc_chaining_hash_table* table)
{
	return table->capacity;
}


/* Drops all items from the table and deallocates used memory. */
void genc_cht_destroy(struct genc_chaining_hash_table* table)
{
	if (table->buckets)
	{
		table->realloc_fn(table->buckets, table->capacity * sizeof(slist_head_t*), 0);
		table->buckets = NULL;
		table->capacity = 0;
		table->item_count = 0;
	}
}

static GENC_INLINE int genc_log2_size(size_t val)
{
	return val == 0 ? -1 : (int)(sizeof(size_t) * 8 - 1 - __builtin_clzll(val));
}

static GENC_INLINE int genc_log2_size_roundup(size_t val)
{
	int log2 = genc_log2_size(val);
	if (genc_is_pow2(val)) return log2;
	return log2 + 1;
}

struct genc_cht_match_ctx
{
	genc_chaining_hash_table_t* table;
	void* key;
};
typedef struct genc_cht_match_ctx genc_cht_match_ctx_t;

/* predicate function for searching a chain for a matching item */
static int genc_item_matches_key(struct slist_head* entry, void* data)
{
	genc_cht_match_ctx_t* ctx = GENC_CXX_CAST(genc_cht_match_ctx_t*, data);
	genc_chaining_hash_table_t* table = ctx->table;
	void* op = table->opaque;
	void* entry_key = table->get_key_fn(entry, op);
	return table->key_equality_fn(entry_key, ctx->key, op);
}

/* Inserts the given item into the hash table.
 * Returns 0 to report failure due to a duplicate, 1 on success. */
int genc_cht_insert_item(struct genc_chaining_hash_table* table, struct slist_head* item)
{
	unsigned new_load;
	if (!item) return 0;
	
	new_load = (unsigned)(100ul * (table->item_count + 1ul) / table->capacity);
	if (new_load > table->load_percent_grow_threshold)
	{
		int factor_log2 = genc_log2_size(new_load / table->load_percent_grow_threshold);
		if (new_load > ((unsigned)table->load_percent_grow_threshold) << factor_log2)
			++factor_log2;
		
		/*printf("New load factor %d%%, growth threshold reached. Growing by factor 1 << %d (%u).\n", new_load, factor_log2, 1u << factor_log2);*/
		genc_cht_grow_by(table, factor_log2);
	}
	
	{
		slist_head_t* found = NULL;
		genc_cht_match_ctx_t ctx;
		void* op = table->opaque;
		void* key = table->get_key_fn(item, op);
		genc_hash_t hash = table->hash_fn(key, op);
		size_t idx = hash & (table->capacity - 1ul);
	
		ctx.table = table;
		ctx.key = key;
		
		found = genc_slist_find_entry(table->buckets[idx], genc_item_matches_key, &ctx);
		if (found)
		{
			/* duplicate exists */
			return 0;
		}
		/* insert at beginning of chain */
		genc_slist_insert_at(item, table->buckets + idx);
		++table->item_count;
	}
	return 1;
}

/* Looks up the key in the table, returning the matching item if present, or NULL otherwise. */
struct slist_head* genc_cht_find(struct genc_chaining_hash_table* table, void* key)
{
	if (!key)
		return NULL;
	return *genc_cht_find_ref(table, key);
}

/* Looks up the key in the table, returning the reference pointing to the
 * matching item, or NULL if not found. The reference may be passed to
 * genc_cht_find_ref() for efficient removal. */
struct slist_head** genc_cht_find_ref(struct genc_chaining_hash_table* table, void* key)
{
	void* op = table->opaque;
	genc_hash_t hash = 0;
	size_t idx;
	genc_cht_match_ctx_t ctx;

	ctx.table = table;
	ctx.key = key;
	
	hash = table->hash_fn(key, op);
	idx = hash & (table->capacity - 1ul);
	
	
	return genc_slist_find_entry_ref(table->buckets + idx, genc_item_matches_key, &ctx);
}

/* Removes the item referred to by item_ref from the hash table, returning it.
 * Deallocation, like allocation, is the responsibility of the caller.
 */
struct slist_head* genc_cht_remove_ref(struct genc_chaining_hash_table* table, struct slist_head** item_ref)
{
	slist_head_t* removed = genc_slist_remove_at(item_ref);
	if (removed)
	{
		unsigned new_load = 0;
		--table->item_count;
		new_load = (unsigned)(100ull * (table->item_count) / table->capacity);

		if (new_load > 0 && new_load < table->load_percent_shrink_threshold)
		{
			int factor_log2 = genc_log2_size(table->load_percent_shrink_threshold / new_load);
			genc_cht_shrink_by(table, factor_log2);
		}
	}
	return removed;
}

/* Calls genc_cht_find_ref() and calls genc_cht_remove_ref() with the result if a match was found. */
struct slist_head* genc_cht_remove(struct genc_chaining_hash_table* table, void* key)
{
	slist_head_t** found = genc_cht_find_ref(table, key);
	if (found && *found)
		return genc_cht_remove_ref(table, found);
	return NULL;
}

/* Shrink the capacity of the table by a factor of 1 << log2_shrink_factor */
void genc_cht_shrink_by(struct genc_chaining_hash_table* table, unsigned log2_shrink_factor)
{
	size_t new_capacity = 0;
	slist_head_t** buckets = NULL;
	
	if (table->capacity <= 1 || log2_shrink_factor == 0)
		return;
	/*  */
	new_capacity = table->capacity >> log2_shrink_factor;
	if (new_capacity < 1)
	{
		new_capacity = 1;
	}
	
	buckets = table->buckets;
		
	{
		/* for each bucket chain that's going to disappear, attach to the "colliding"
		 * bucket at the beginning of the table that will still exist. */
		size_t mask = new_capacity - 1;
		size_t i;
		for (i = new_capacity; i < table->capacity; ++i)
		{
			genc_slist_splice(&buckets[i & mask], buckets + i);
		}
	}
	
	table->buckets = GENC_CXX_CAST(slist_head_t**, table->realloc_fn(buckets, table->capacity * sizeof(slist_head_t*), new_capacity * sizeof(slist_head_t*)));
	table->capacity = new_capacity;
}

/* Grow the capacity of the table by a factor of 1 << log2_grow_factor  */
void genc_cht_grow_by(struct genc_chaining_hash_table* table, unsigned log2_grow_factor)
{
	slist_head_t** buckets = NULL;
	size_t old_capacity = 0;
	size_t new_capacity = table->capacity << log2_grow_factor;
	while (new_capacity < table->capacity)
	{
		/* integer overflow */
		--log2_grow_factor;
		new_capacity = table->capacity << log2_grow_factor;
	}
	if (new_capacity == table->capacity)
		return;

	buckets = GENC_CXX_CAST(slist_head_t**, table->realloc_fn(table->buckets, table->capacity * sizeof(slist_head_t*), new_capacity * sizeof(slist_head_t*)));
	if (!buckets)
		return;
		
	old_capacity = table->capacity;
	GENC_MEMSET(buckets + old_capacity, 0, (new_capacity - old_capacity) * sizeof(buckets[0]));
	
	table->buckets = buckets;
	table->capacity = new_capacity;
	
	{
		void* op = table->opaque;
		genc_chaining_hash_get_item_key_fn get_key = table->get_key_fn;
		genc_chaining_key_hash_fn key_hash = table->hash_fn;
	
		/* Re-hash each existing bucket chain. */
		size_t mask = new_capacity - 1;
		size_t i;
		for (i = 0; i < old_capacity; ++i)
		{
			slist_head_t* cur = NULL;
			slist_head_t** cur_ref = buckets + i;
			genc_slist_for_each_head_ref(cur, cur_ref)
			{
				while (cur) /* Need this as we'd end up skipping an item after every one we remove otherwise */
				{
					void* key = get_key(cur, op);
					genc_hash_t hash = key_hash(key, op);
					size_t idx = hash & mask;
				
					if (idx == i)
						break;
					/* remove from chain, add to new bucket */
					genc_slist_remove_at(cur_ref);
					genc_slist_insert_at(cur, buckets + idx);
					cur = *cur_ref;
				}
			}
		}
	}
}
