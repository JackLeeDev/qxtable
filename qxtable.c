#define LUA_LIB

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <lua.h>
#include <lauxlib.h>
#include "qlock.h"
#include "qbuf.h"
#include "qbarray.h"

#define QXT_TNIL 0
#define QXT_TINTEGER 1
#define QXT_TNUMBER 2
#define QXT_TSTRING 3
#define QXT_TBOOLEAN 4
#define QXT_TTABLE 5

#define QXT_TT_MAP 0
#define QXT_TT_ARRAY 1
#define QXT_TT_I32ARRAY 2

#define STRING_HNODE_SIZE 40960
#define TABLE_HNODE_SIZE 10240

#define get_kv_type(tb, idx) (*((uint8_t*)tb-(idx+1)))
#define get_kv_typep(tb, idx) ((uint8_t*)tb-(idx+1))
#define get_table_raw_ptr(tb) (void*)((uint8_t*)tb - tb->size)
#define get_table_node_element(tb, idx)  (&(((qxtable_node*)((tb)+1))[idx]))
#define get_table_array_element(tb, idx) (&(((qxtable_value*)((tb)+1))[idx]))
#define get_table_i32array_element(tb, idx) (&(((int32_t*)((tb)+1))[idx]))

typedef struct qxtable_value {
	union {
		int64_t iv;
		double dv;
		bool bv;
		const char* sv;
		struct qxtable* tv;
	};
} qxtable_value;

typedef struct qxtable_node {
	union {
		int64_t ik;
		const char* sk;
	};
	union {
		int64_t iv;
		double dv;
		bool bv;
		const char* sv;
		struct qxtable* tv;
	};
} qxtable_node;

typedef struct qxtable {
	// flexible array, stores element type data
	uint8_t tt : 2;
	uint32_t size : 30;
	// flexible array, stores element data
} qxtable;

typedef struct config {
	const char* name;
	qxtable* tb;
	struct config* next;
} config;

typedef struct qxtable_hnode {
	qbarray harray;
	qbarray array;
} qxtable_hnode;

typedef struct string_node {
	qbarray array;
	rwlock_t lock;
} string_node;

typedef struct tostring_encoder {
	char* buffer;
	int32_t cap;
	int32_t write_ptr;
} tostring_encoder;

typedef struct config_context {
	config* cfg;
	string_node str_hnode[STRING_HNODE_SIZE];
	qxtable_hnode tb_hnode[TABLE_HNODE_SIZE];
	size_t mem_size;
	spinlock_t lock;
	bool inited;
} config_context;

static config_context g_ctx;

static inline void* qxtable_malloc(size_t size) {
	g_ctx.mem_size += size;
	return malloc(size);
}

static inline void* qxtable_realloc(void* data, size_t osize, size_t nsize) {
	g_ctx.mem_size -= osize;
	g_ctx.mem_size += nsize;
	return realloc(data, nsize);
}

static inline void qxtable_free(void* data, size_t size) {
	g_ctx.mem_size -= size;
	free(data);
}

static inline const char* qxtable_strdup(const char* str, size_t len) {
	char* new_str = (char*)qxtable_malloc(len + 1);
	memcpy(new_str, str, len + 1);
	return new_str;
}

static inline uint32_t string_hash(const char *str, size_t len) {
    uint32_t h = 0x811c9dc5;
    uint32_t i;
    for (i = 0; i < len; i++) {
        h ^= (uint32_t)(uint8_t)str[i];
        h *= 0x01000193;
    }
    return h;
}

static const char* string_cache(const char* str) {
	size_t len = strlen(str);
	uint32_t h = string_hash(str, len);
	uint32_t node_index = h % STRING_HNODE_SIZE;
	string_node* node = &g_ctx.str_hnode[node_index];

	rwlock_t* lock = &node->lock;

	rwlock_rlock(lock);
		int32_t idx = qbarray_indexof(&node->array, &str);
		if (idx >= 0) {
			str = *(const char**)qbarray_get(&node->array, idx);
			rwlock_runlock(lock);
			return str;
		}
	rwlock_runlock(lock);

	rwlock_wlock(lock);
		str = qxtable_strdup(str, len);
		bool ok = qbarray_insert(&node->array, &str);
		assert(ok);
	rwlock_wunlock(lock);

	return str;
}

static inline int32_t get_current_size(qxtable* tb) {
	int32_t i;
	int32_t size = 0;
	for (i = 0; i < tb->size; ++i) {
		if (get_kv_type(tb, i) != 0)
			++size;
		else
			break;
	}
	return size;
}

static inline int32_t table_info(lua_State *L, uint8_t* tt) {
	int32_t size = 0;
	bool is_array = true;
	int32_t array_size = lua_rawlen(L, -1);
	lua_pushnil(L);
	while (lua_next(L, -2)) {
		if (is_array) {
			if (lua_isinteger(L, -2)){
				int64_t ik = lua_tointeger(L, -2);
				if (ik < 1 || ik > array_size) {
					is_array = false;
				}
			}
			else {
				is_array = false;
			}
		}
		++size;
		lua_pop(L, 1);
	}
	*tt = (is_array && size==array_size) ? QXT_TT_ARRAY : QXT_TT_MAP;
	return size;
}

static inline uint8_t get_key_type_by_index(qxtable* tb, int32_t idx) {
	assert(idx >= 0 && idx < tb->size);
	if (tb->tt == QXT_TT_MAP) {
		uint8_t type = get_kv_type(tb, idx);
		return type >> 4;
	}
	else {
		return QXT_TINTEGER;
	}
}

static inline uint8_t get_key_by_index(qxtable* tb, int32_t idx, int64_t* ik, const char** sk) {
	int32_t tk = get_key_type_by_index(tb, idx);
	assert(tk != QXT_TNIL);
	if (tb->tt == QXT_TT_MAP) {
		if (tk == QXT_TINTEGER) {
			*ik =  get_table_node_element(tb, idx)->ik;
		}
		else {
			*sk = get_table_node_element(tb, idx)->sk;
		}
	}
	else {
		assert(tk == QXT_TINTEGER);
		*ik = idx + 1;
	}
	return tk;
}

static inline uint8_t get_value_type_by_index(qxtable* tb, int32_t idx) {
	uint8_t type = get_kv_type(tb, idx);
	return tb->tt==QXT_TT_MAP ? (type&0xF) : type;
}

static uint8_t get_value_by_index(qxtable* tb, int32_t idx, qxtable_value* value) {
	if (idx < 0 || idx >= tb->size) {
		return QXT_TNIL;
	}
	uint8_t tv = get_value_type_by_index(tb, idx);
	uint8_t tt = tb->tt;
	if (tt == QXT_TT_MAP) {
		value->iv = get_table_node_element(tb, idx)->iv;
	}
	else if (tt == QXT_TT_I32ARRAY) {
		value->iv = *get_table_i32array_element(tb, idx);
	}
	else if (tt == QXT_TT_ARRAY) {
		value->iv = get_table_array_element(tb, idx)->iv;
	}
	else {
		assert(0);
	}
	return tv;
}

static inline int32_t key_compare(int64_t ik1, const char* sk1, int64_t ik2, const char* sk2) {
	if (sk1) {
		return sk2 ? strcmp(sk1, sk2) : 1;
	}
	else {
		return sk2 ? -1 : ik1 - ik2;
	}
}

static int32_t _map_field_index(qxtable* tb, int64_t ik, const char* sk, int32_t* ins_idx, bool insert) {
	assert(tb->tt == QXT_TT_MAP);
	int32_t left = 0;
	int32_t right = !insert ? (tb->size-1) : (get_current_size(tb) - 1);
	while (left <= right) {
		int64_t ik2 = 0;
		const char* sk2 = NULL;
		int32_t middle = (left + right) / 2;
		get_key_by_index(tb, middle, &ik2, &sk2);
		int32_t ret = key_compare(ik, sk, ik2, sk2);
		if (ret < 0)
			right = middle - 1;
		else if (ret == 0)
			return middle;
		else
			left = middle + 1;
	}
	if (ins_idx)
		*ins_idx = left;
	return -1;
}

static inline int32_t find_key_index(qxtable* tb, int64_t ik, const char* sk) {
	if (tb->tt == QXT_TT_MAP) {
		int32_t idx = _map_field_index(tb, ik, sk, NULL, false);
		if (idx >= 0)
			return idx;
	}
	else {
		if (ik > 0 && ik <= tb->size)
			return ik - 1;
	}
	return -1;
}

static inline int32_t find_map_insert_index(qxtable* tb, int64_t ik, const char* sk) {
	assert(tb->tt == QXT_TT_MAP);
	int32_t ins_idx = -1;
	int32_t idx = _map_field_index(tb, ik, sk, &ins_idx, true);
	assert(idx<0 && ins_idx>=0);
	return ins_idx;
}

static inline void set_value_type(qxtable* tb, int32_t idx, uint8_t tv) {
	assert(idx >= 0 && idx < tb->size);
	uint8_t tk = get_kv_type(tb, idx) >> 4;
	*get_kv_typep(tb, idx) = (tk << 4) | tv;
	assert(get_value_type_by_index(tb, idx) == tv);
}

static inline void set_kv_type(qxtable* tb, int32_t idx, uint8_t kv_type) {
	assert(idx >= 0 && idx < tb->size);
	*get_kv_typep(tb, idx) = kv_type;
}

static inline void insert_array_value(qxtable* tb, int64_t ik, int32_t tv, qxtable_value* value) {
	uint8_t tt = tb->tt;
	assert(tt != QXT_TT_MAP);
	assert(ik > 0 && ik <= tb->size);
	assert(tv != QXT_TNIL);
	int32_t idx = ik - 1;
	assert(get_value_type_by_index(tb, idx) == QXT_TNIL);
	set_value_type(tb, idx, tv);
	if (tt == QXT_TT_I32ARRAY) {
		*get_table_i32array_element(tb, idx)= (int32_t)value->iv;
	}
	else if (tt == QXT_TT_ARRAY) {
		get_table_array_element(tb, idx)->iv = value->iv;
	}
	else {
		assert(0);
	}
}

static void insert_map_value(qxtable* tb, int64_t ik, const char* sk, uint8_t tv, qxtable_value* value) {
	assert(tb->tt == QXT_TT_MAP);
	assert(tv != QXT_TNIL);

	int32_t idx = find_map_insert_index(tb, ik, sk);
	assert(idx >= 0);

	int32_t current_size = get_current_size(tb);
	int32_t move_size = current_size - idx;
	if (move_size > 0) {
		uint8_t* type_data = get_kv_typep(tb, current_size - 1);
		memmove(type_data - 1, type_data, move_size);
		qxtable_node* node = get_table_node_element(tb, idx);
		memmove(node + 1, node, move_size * sizeof(*node));
	}

	uint8_t kv_type = ((sk ? QXT_TSTRING : QXT_TINTEGER) << 4) | tv;
	set_kv_type(tb, idx, kv_type);
	if (sk) {
		get_table_node_element(tb, idx)->sk = string_cache(sk);
	}
	else {
		get_table_node_element(tb, idx)->ik = ik;
	}
	get_table_node_element(tb, idx)->iv = value->iv;
}

static inline int32_t push_key_by_index(lua_State *L, qxtable* tb, int32_t idx) {
	assert(idx >= 0 && idx < tb->size);
	int64_t ik = 0;
	const char* sk = NULL;
	uint8_t tk = get_key_by_index(tb, idx, &ik, &sk);
	assert(tk != QXT_TNIL);
	if (sk) {
		lua_pushstring(L, sk);
	}
	else {
		lua_pushinteger(L, ik);
	}
	return 1;
}

static int32_t push_value(lua_State *L, uint8_t tv, qxtable_value* value) {
	switch (tv) {
		case QXT_TINTEGER: {
			lua_pushinteger(L, value->iv);
			break;
		}
		case QXT_TNUMBER: {
			lua_pushnumber(L, value->dv);
			break;
		}
		case QXT_TBOOLEAN: {
			lua_pushboolean(L, value->bv ? 1 : 0);
			break;
		}
		case QXT_TSTRING: {
			lua_pushstring(L, value->sv);
			break;
		}
		case QXT_TTABLE: {
			lua_pushnil(L);
			lua_pushlightuserdata(L, value->tv);
			return 2;
		}
		default: {
			return 0;
		}
	}
	return 1;
}

static inline int32_t push_value_by_index(lua_State *L, qxtable* tb, int32_t idx) {
	assert(idx >= 0 && idx < tb->size);
	qxtable_value value;
	uint8_t tv = get_value_by_index(tb, idx, &value);
	assert(tv != QXT_TNIL);
	return push_value(L, tv, &value);
}

static bool table_equal(qxtable* tb1, qxtable* tb2) {
	assert(tb1 != tb2);

	uint8_t tt = tb1->tt;
	if (tt != tb2->tt) {
		return false;
	}

	int32_t size = tb1->size;
	if (size != tb2->size) {
		return false;
	}
	
	if (size == 0) {
		return true;
	}

	uint8_t* type1 = get_kv_typep(tb1, size - 1);
	uint8_t* type2 = get_kv_typep(tb2, size - 1);
	if (memcmp(type1, type2, size) != 0) {
		return false;
	}
	
	void* elem_ptr = tb1 + 1;
	if (tt == QXT_TT_MAP) {
		if (memcmp(elem_ptr, get_table_node_element(tb2, 0), size*sizeof(qxtable_node)) != 0) {
			return false;
		}
	}
	else if (tt == QXT_TT_I32ARRAY) {
		if (memcmp(elem_ptr, get_table_i32array_element(tb2, 0), size*sizeof(int32_t)) != 0) {
			return false;
		}
	}
	else if (tt == QXT_TT_ARRAY) {
		if (memcmp(elem_ptr, get_table_array_element(tb2, 0), size*sizeof(qxtable_value)) != 0) {
			return false;
		}
	}
	else {
		assert(0);
	}

	return true;
}

static inline qxtable* find_node_table(qxtable* tb, uint32_t h) {
	uint32_t idx = h % TABLE_HNODE_SIZE;
	qxtable_hnode* node = &g_ctx.tb_hnode[idx];
	int32_t i;
	for (i = 0; i < node->array.size; ++i) {
		if (h == *(uint32_t*)qbarray_get(&node->harray, i)) {
			qxtable* cache = *(qxtable**)qbarray_get(&node->array, i);
			if (table_equal(tb, cache)) {
				return cache;
			}
		} 
	}
	return NULL;
}

static inline int32_t table_mem_size(int32_t size, uint8_t tt) {
	int32_t mem_size = size + sizeof(qxtable);
	if (size > 0) {
		if (tt == QXT_TT_MAP) {
			mem_size += sizeof(qxtable_node) * size;
		}
		else if (tt == QXT_TT_ARRAY) {
			mem_size += sizeof(qxtable_value) * size;
		}
		else if (tt == QXT_TT_I32ARRAY) {
			mem_size += sizeof(int32_t) * size;
		}
		else {
			assert(0);
		}
	}
	return mem_size;
}

static inline void shallow_free_table(qxtable* tb) {
	void* p = get_table_raw_ptr(tb);
	int32_t mem_size = table_mem_size(tb->size, tb->tt);
	qxtable_free(p, mem_size);
}

static inline qxtable* new_table(int32_t size, uint8_t tt) {
	int32_t mem_size = table_mem_size(size, tt);
	qxtable* tb = qxtable_malloc(mem_size);
	memset(tb, 0, mem_size);
	tb = (qxtable*)((uint8_t*)tb + size);
	tb->tt = tt;
	tb->size = size;
	return tb;
}

static qxtable* convert_table(lua_State *L, int32_t* rh) {
	uint8_t tt = 0;
	int32_t size = table_info(L, &tt);
	uint32_t h = 0;
	int32_t int32_num = 0;

	qxtable* tb = new_table(size, tt);

	lua_pushnil(L);
	while (lua_next(L, -2)) {
		int64_t ik = 0;
		const char* sk = NULL;

		int32_t tk = lua_type(L, -2);
		if (tk == LUA_TNUMBER) {
			ik = lua_tointeger(L, -2);
			h += ik;
		}
		else if (tk == LUA_TSTRING) {
			if (tt == QXT_TT_MAP) {
				sk = lua_tostring(L, -2);
				h ^= strlen(sk);
				h *= 0x01000193;
			}
			else {
				luaL_error(L, "Array invalid key type(string)");
			}
		}
		else {
			luaL_error(L, "Invalid key type(%s)", luaL_typename(L, -2));
		}

		int32_t tv = 0;
		qxtable_value value;

		int32_t ltv = lua_type(L, -1);
		switch (ltv) {
			case LUA_TNUMBER: {
				if (lua_isinteger(L, -1)) {
					tv = QXT_TINTEGER;
					value.iv = lua_tointeger(L, -1);
					h += (int32_t)value.iv;
					if (value.iv >= -0x7FFFFFFF && value.iv <= 0x7FFFFFFF) {
						++int32_num;
					}
				}
				else {
					tv = QXT_TNUMBER;
					value.dv = lua_tonumber(L, -1);
					h = (int32_t)value.dv;
				}
				break;
			}
			case LUA_TBOOLEAN: {
				tv = QXT_TBOOLEAN;
				value.bv = lua_toboolean(L, -1);
				h += (int32_t)value.bv;
				break;
			}
			case LUA_TSTRING: {
				tv = QXT_TSTRING;
				value.sv = string_cache(lua_tostring(L, -1));
				h += (int32_t)strlen(value.sv);
				break;
			}
			case LUA_TTABLE: {
				tv = QXT_TTABLE;
				int32_t sub_hash = 0;
				qxtable* tb = convert_table(L, &sub_hash);
				value.tv = tb;
				h += sub_hash;
				h *= 0x01000193;
				break;
			}
			default: {
				luaL_error(L, "Invalid value type(%s)", luaL_typename(L, -1));
				break;
			}
		}

		if (tt == QXT_TT_MAP) {
			insert_map_value(tb, ik, sk, tv, &value);
		}
		else {
			insert_array_value(tb, ik, tv, &value);
			h ^= 1;
		}

		lua_pop(L, 1);
	}

	if (size > 0 && tt == QXT_TT_ARRAY) {
		if (int32_num == size) {
			qxtable* new_tb = new_table(size, QXT_TT_I32ARRAY);
			int32_t i;
			for (i = 0; i < size; ++i) {
				*get_kv_typep(new_tb, i) = get_kv_type(tb, i);
				*get_table_i32array_element(new_tb, i) = (int32_t)get_table_array_element(tb, i)->iv;
			}
			shallow_free_table(tb);
			tb = new_tb;
		}
	}

	assert(get_current_size(tb) == tb->size);

	qxtable* cache = find_node_table(tb, h);
	if (cache) {
		shallow_free_table(tb);
		tb = cache;
	}
	else {
		uint32_t idx = h % TABLE_HNODE_SIZE;
		qxtable_hnode* node = &g_ctx.tb_hnode[idx];
		qbarray_push_back(&node->harray, &h);
		qbarray_push_back(&node->array, &tb);
	}

	if (rh) {
		*rh = h;
	}

	return tb;
}

static inline void save_config(config* ncfg) {
	config* cfg = g_ctx.cfg;
	while (cfg) {
		if (strcmp(cfg->name, ncfg->name) == 0) {
			break;
		}
		cfg = cfg->next;
	}
	if (cfg) {
		qxtable_free(ncfg, sizeof(*ncfg));
	}
	else {
		ncfg->next = g_ctx.cfg;
		g_ctx.cfg = ncfg;
	}	
}

static int32_t lindex(lua_State *L) {
	qxtable* tb = lua_touserdata(L, 1);
	if (!tb) {
		return luaL_error(L, "Invalid userdata");
	}

	int64_t ik = 0;
	const char* sk = NULL;

	int32_t tk = lua_type(L, 2);
	switch (tk) {
		case LUA_TSTRING: {
			sk = lua_tostring(L, 2);
			break;
		}
		case LUA_TNUMBER: {
			ik = lua_tointeger(L, 2);
			break;
		}
		case LUA_TNIL: {
			return 0;
		}
		default: {
			luaL_error(L, "Invalid key type(%s)", luaL_typename(L, 2));
			break;
		}
	}
	
	if (tb->size > 0) {
		int32_t idx = find_key_index(tb, ik, sk);
		if (idx >= 0) {
			return push_value_by_index(L, tb, idx);
		}
	}

	return 0;
}

static int32_t llen(lua_State *L) {
	qxtable* tb = lua_touserdata(L, 1);
	if (!tb) {
		return luaL_error(L, "Invalid userdata");
	}

	if (tb->tt == QXT_TT_MAP) {
		int32_t array_size = 0;
		int32_t i;
		for (i = 1; i <= tb->size; ++i) {
			if (find_key_index(tb, i, NULL) >= 0) {
				++array_size;
			}
			else {
				break;
			}
		}
		lua_pushinteger(L, array_size);
	}
	else {
		lua_pushinteger(L, tb->size);
	}

	return 1;
}
	
static int32_t lnext(lua_State *L) {
	qxtable* tb = lua_touserdata(L, 1);
	if (!tb) {
		return luaL_error(L, "Invalid userdata");
	}
	
	int64_t ik = 0;
	const char* sk = NULL;
	int32_t next_index = -1;
	int32_t type = lua_type(L, 2);

	if (type == LUA_TNUMBER) {
		ik = lua_tointeger(L, 2);
	}
	else if (type == LUA_TSTRING) {
		sk = lua_tostring(L, 2);
	}
	else if (type == LUA_TNIL) {
		next_index = 0;
	}

	if (next_index < 0) {
		next_index = find_key_index(tb, ik, sk) + 1;
	}
	if (next_index >= 0 && next_index < tb->size) {
		push_key_by_index(L, tb, next_index);
		return push_value_by_index(L, tb, next_index) + 1;
	}
	
	return 0;
}

static bool write_table_string(qxtable* tb, tostring_encoder* encoder) {
	int32_t size = tb->size;
	uint8_t tt = tb->tt;

	write_byte(encoder, tt);
	write_uinteger(encoder, size);

	if (size == 0) {
		return true;
	}

	if (tt == QXT_TT_I32ARRAY) {
		void* p = get_table_raw_ptr(tb);
		write_buf(encoder, p, table_mem_size(size, tt));
		return true;
	}

	uint8_t* td = get_kv_typep(tb, size - 1);
	write_buf(encoder, td, size);

	if (tt == QXT_TT_MAP) {
		int32_t i;
		for (i = 0; i < size; ++i) {
			int64_t ik = 0;
			const char* sk = NULL;
			uint8_t tk = get_key_by_index(tb, i, &ik, &sk);
			if (tk == QXT_TSTRING) {
				int32_t len = (int32_t)strlen(sk);
				write_uinteger(encoder, len);
				write_buf(encoder, sk, len);
			}
			else {
				write_integer(encoder, ik);
			}
		}
	}

	int32_t i;
	for (i = 0; i < size; ++i) {
		qxtable_value value;
		uint8_t tv = get_value_by_index(tb, i, &value);
		switch (tv) {
			case QXT_TINTEGER:
				write_integer(encoder, value.iv);
				break;
			case QXT_TNUMBER:
				write_buf(encoder, &value.dv, sizeof(double));
				break;
			case QXT_TBOOLEAN:
				write_byte(encoder, value.bv ? 1 : 0);
				break;
			case QXT_TSTRING: {
				int32_t len = (int32_t)strlen(value.sv);
				write_uinteger(encoder, len);
				write_buf(encoder, value.sv, len);
				break;
			}
			case QXT_TTABLE:
				if (!write_table_string(value.tv, encoder)) {
					return false;
				}
				break;
			default:
				return false;
		}
	}

	return true;
}

static int32_t ltostring(lua_State *L) {
	qxtable* tb = lua_touserdata(L, 1);
	if (!tb) {
		return luaL_error(L, "Invalid userdata");
	}
	
	tostring_encoder encoder;
	memset(&encoder, 0, sizeof(encoder));
	if (!write_table_string(tb, &encoder)) {
		if (encoder.buffer) {
			free(encoder.buffer);
		}
		luaL_error(L, "tostring error");
		return 0;
	}

	assert(encoder.buffer);
	lua_pushlstring(L, encoder.buffer, encoder.write_ptr);
	free(encoder.buffer);
	
	return 1;
}

static int32_t string_compare(const void* a, const void* b) {
    return strcmp(*(const char**)a, *(const char**)b);
}

static void g_ctx_init() {
	assert(!g_ctx.inited);
	g_ctx.inited = true;

	int32_t i;
	for (i=0; i<STRING_HNODE_SIZE; ++i) {
		string_node* node = &g_ctx.str_hnode[i];
		qbarray_init(&node->array, sizeof(const char*), 2, string_compare);
	}

	for (i=0; i<TABLE_HNODE_SIZE; ++i) {
		qxtable_hnode* node = &g_ctx.tb_hnode[i];
		qbarray_init(&node->harray, sizeof(uint32_t), 2, NULL);
		qbarray_init(&node->array, sizeof(qxtable*), 2, NULL);
	}
}

static int32_t lupdate(lua_State *L) {
	if (!g_ctx.inited) {
		g_ctx_init();
	}

	config* cfg = NULL;
	
	lua_pushnil(L);
	while (lua_next(L, -2)) {
		const char* name = lua_tostring(L, -2);
		qxtable* tb = convert_table(L, NULL);
		config* ncfg = qxtable_malloc(sizeof(*ncfg));
		ncfg->name = string_cache(name);
		ncfg->tb = tb;
		ncfg->next = cfg;
		cfg = ncfg;
		lua_pop(L, 1);
	}

	spin_lock(&g_ctx.lock);
		while (cfg) {
			config* next = cfg->next;
			cfg->next = NULL;
			save_config(cfg);
			cfg = next;
		}
	spin_unlock(&g_ctx.lock);
		
	return 0;
}

static int32_t lreload(lua_State *L) {
	lua_newtable(L);
	spin_lock(&g_ctx.lock);
		config* cfg = g_ctx.cfg;
		while (cfg) {
			if (cfg->tb) {
				lua_pushstring(L, cfg->name);
				lua_pushlightuserdata(L, cfg->tb);
				lua_rawset(L, -3);
			}
			cfg = cfg->next;
		}
	spin_unlock(&g_ctx.lock);
	return 1;
}

static int32_t lmemory(lua_State *L) {
	lua_pushinteger(L, g_ctx.mem_size);
	return 1;
}

int32_t luaopen_qxtable_core(lua_State *L) {
	luaL_checkversion(L);
	luaL_Reg l[] = {
		{ "index",		lindex		},
		{ "len",		llen		},
		{ "next",		lnext		},
		{ "tostring",	ltostring	},
		{ "update",		lupdate		},
		{ "reload",		lreload		},
		{ "memory",		lmemory		},
		{ NULL,			NULL 		},
	};
	luaL_newlib(L, l);
	return 1;
}
