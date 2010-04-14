/*****************************************************************************
 * LArc library
 * Copyright (C) 2010 Tom N Harris. All rights reserved.
 *
 *  This software is provided 'as-is', without any express or implied
 *  warranty.  In no event will the authors be held liable for any damages
 *  arising from the use of this software.
 *
 *  Permission is granted to anyone to use this software for any purpose,
 *  including commercial applications, and to alter it and redistribute it
 *  freely, subject to the following restrictions:
 *
 *  1. The origin of this software must not be misrepresented; you must not
 *     claim that you wrote the original software. If you use this software
 *     in a product, an acknowledgment in the product documentation would be
 *     appreciated but is not required.
 *  2. Altered source versions must be plainly marked as such, and must not be
 *     misrepresented as being the original software.
 *  3. This notice may not be removed or altered from any source distribution.
 *  4. Neither the names of the authors nor the names of any of the software 
 *     contributors may be used to endorse or promote products derived from 
 *     this software without specific prior written permission.
 */
/*
LZMA codec uses a filter chain that needs to be
preconfigured.

lzma.filter("delta",{distance=512}, "lzma2",{dict_size=10240}) -> filter object
filter:encode() -> filter chain encoded in a string
filter:decode() -> build the filter chain described by a string
filter[0]:encode()
filter[0]:rawencode()
filter[0].property
filter:add(...)
filter:addencoded(str)
filter:addraw("lzma1", str)
filter:del(0)

*/
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "lzma.h"
#include "shared.h"

#define UINT64TYPE	"large integer"

#define LZMA_MT	"larc.lzma.stream"
#define LZMAFILTER_LZMA_MT	"larc.lzma.lzmafilter"
#define LZMAFILTER_DELTA_MT	"larc.lzma.deltafilter"
#define LZMAFILTER_BCJ_MT	"larc.lzma.bcjfilter"

static void newuint64 (lua_State *L, uint64_t i) {
  uint64_t *li = (uint64_t*)lua_newuserdata(L, sizeof(uint64_t));
  *li = i;
  luaL_getmetatable(L, UINT64TYPE);
  if (lua_isnil(L, -1))
	luaL_error(L, "struct module not loaded");
  lua_setmetatable(L, -2);
}

static uint64_t getuint64 (lua_State *L, int i) {
  uint64_t li;
  if (lua_isnoneornil(L, i))
    luaL_argerror(L, i, "large integer expected, got no value");
  switch (lua_type(L, i)) {
    default: {
      char *e;
      const char *s = lua_tostring(L, i);
      li = strtoll(s, &e, 0);
      if (e == s || *e != '\0')
        luaL_argerror(L, i, "invalid string for integer");
      return li;
    }
    case LUA_TNUMBER:
      li = luaL_checknumber(L, i);
      return li;
    case LUA_TUSERDATA:
    {
      li = *((uint64_t*)luaL_checkudata(L, i, UINT64TYPE));
      return li;
    }
  }
  return 0;
}

static const int status_to_errcode[] = {
	LZMA_OK,
	LZMA_STREAM_END,
	LZMA_NO_CHECK,
	LZMA_UNSUPPORTED_CHECK,
	LZMA_GET_CHECK,
	-LZMA_MEM_ERROR,
	-LZMA_MEMLIMIT_ERROR,
	-LZMA_FORMAT_ERROR,
	-LZMA_OPTIONS_ERROR,
	-LZMA_DATA_ERROR,
	-LZMA_BUF_ERROR,
	-LZMA_PROG_ERROR,
};

static const char * const status_to_string[] = {
	"Operation completed successfully",
	"End of stream was reached",
	"Input stream has no integrity check",
	"Cannot calculate the integrity check",
	"Integrity check type is now available",
	"Cannot allocate memory",
	"Memory usage limit was reached",
	"File format not recognized",
	"Invalid or unsupported options",
	"Data is corrupt",
	"No progress is possible",
	"Programming error",
};

typedef struct lzma_userdata
{
	lzma_stream z;
	int status;
	int result;
	lzma_action flush;
} z_userdata;

typedef struct lzmafilter_userdata
{
	lzma_filter head; /* ID and pointer to options */
	lzma_vli end; /* set to LZMA_VLI_UNKNOWN */
	union {
		lzma_options_lzma lzma;
		lzma_options_delta delta;
		lzma_options_bcj bcj;
	} options;
} filter_userdata;

static filter_userdata * new_lzmafilter(lua_State *L, lzma_vli id, const char * mt)
{
	filter_userdata *ud = (filter_userdata*)lua_newuserdata(L, sizeof(filter_userdata));
	luaL_getmetatable(L, mt);
	lua_setmetatable(L, -2);
	memset(ud, 0, sizeof(filter_userdata));
	ud->head.id = id;
	ud->head.options = &ud->options;
	ud->end = LZMA_VLI_UNKNOWN;
	return ud;
}

static filter_userdata * get_lzmafilter(lua_State *L, int n)
{
	filter_userdata *ud;
	if (lua_isnoneornil(L, n))
		luaL_error(L, "lzma filter expected");
	ud = (filter_userdata*)lua_touserdata(L, n);
	if (ud == NULL)
		luaL_error(L, "lzma filter expected");
	return ud;
}

static const char * const filter_options[] = {
	"",
	"lzma1",
	"lzma2",
	"delta",
	"x86",
	"ia64",
	"arm",
	"armthumb",
	"powerpc",
	"sparc",
	NULL
};
static const lzma_vli filter_ids[] = {
	LZMA_VLI_UNKNOWN,
	LZMA_FILTER_LZMA1,
	LZMA_FILTER_LZMA2,
	LZMA_FILTER_DELTA,
	LZMA_FILTER_X86,
	LZMA_FILTER_IA64,
	LZMA_FILTER_ARM,
	LZMA_FILTER_ARMTHUMB,
	LZMA_FILTER_POWERPC,
	LZMA_FILTER_SPARC,
};

/**
 * Static allocator that returns the existing buffer.
 */
typedef struct lzmafilter_alloc_struct
{
	lzma_allocator lzma;
	void *buf;
	size_t len;
} lzmafilter_alloc;
static void * lzmafilter_static_alloc(void *ptr, size_t n, size_t sz)
{
	lzmafilter_alloc *alloc = (lzmafilter_alloc *)ptr;
	return alloc->buf;
}

static void lzmafilter_static_free(void *ptr, void *buf)
{
	/* do nothing */
}

static uint32_t lzmafilter_optint(lua_State *L, int argt, const char *opt, uint32_t def)
{
	lua_getfield(L, argt, opt);
	if (!lua_isnil(L, -1))
	{
		def = lua_tonumber(L, -1);
		if (def == 0 && !lua_isnumber(L, -1))
			luaL_argerror(L, argt, "invalid filter option");
	}
	lua_pop(L, 1);
	return def;
}

static int lzmafilter_option(lua_State *L, int argt, const char *opt, const char * const options[])
{
	const char *s;
	int i;
	int ret = 0;
	lua_getfield(L, argt, opt);
	if (!lua_isnil(L, -1))
	{
		s = lua_tostring(L, -1);
		if (s == NULL)
			luaL_argerror(L, argt, "invalid filter option");
		ret = -1;
		for (i=0; options[i]; i++)
			if (0 == strcmp(options[i], s))
			{
				ret = i;
				break;
			}
		if (ret == -1)
			luaL_argerror(L, argt, lua_pushfstring(L, "%s \"%s\"", "invalid filter option", s));
	}
	lua_pop(L, 1);
	return ret;
}

static const char * const lzmamode_opts[] = { "normal","fast",NULL };
static const lzma_mode lzmamode_ids[] = { LZMA_MODE_NORMAL,LZMA_MODE_FAST };
static const char * const matchfinder_opts[] = { "bt4","bt3","bt2","hc4","hc3",NULL };
static const lzma_match_finder matchfinder_ids[] = { LZMA_MF_BT4,LZMA_MF_BT3,LZMA_MF_BT2,LZMA_MF_HC4,LZMA_MF_HC3 };

/**
 * Create a filter.
 */
static int larc_lzmafilter_new(lua_State *L)
{
	lzmafilter_alloc alloc;
	lzma_ret status;
	filter_userdata *filter;
	int ftype = luaL_checkoption(L, 1, NULL, filter_options);
	const char *str = NULL;
	size_t len;
	alloc.lzma.alloc = lzmafilter_static_alloc;
	alloc.lzma.free = lzmafilter_static_free;
	alloc.lzma.opaque = &alloc;
	lua_settop(L, 2);
	if (!lua_istable(L, 2))
		str = luaL_optlstring(L, 2, NULL, &len);
	switch (ftype)
	{
	default:
		return luaL_argerror(L, 1, "unsupported filter");
	case 1:
	case 2:
		filter = new_lzmafilter(L, filter_ids[ftype], LZMAFILTER_LZMA_MT);
		filter->options.lzma.dict_size = LZMA_DICT_SIZE_DEFAULT;
		filter->options.lzma.lc = LZMA_LC_DEFAULT;
		filter->options.lzma.lp = LZMA_LP_DEFAULT;
		filter->options.lzma.pb = LZMA_PB_DEFAULT;
		filter->options.lzma.mode = LZMA_MODE_NORMAL;
		filter->options.lzma.nice_len = 64;
		filter->options.lzma.mf = LZMA_MF_BT4;
		filter->options.lzma.depth = 0;
		if (str != NULL)
		{
			alloc.buf = &filter->options;
			alloc.len = sizeof(lzma_options_lzma);
			status = lzma_properties_decode(&filter->head, &alloc.lzma, (const uint8_t*)str, len);
			if (status != LZMA_OK)
				luaL_argerror(L, 2, status_to_string[status]);
		}
		else if (lua_istable(L, 2))
		{
			int o = lzmafilter_option(L, 2, "mode", lzmamode_opts);
			filter->options.lzma.mode = lzmamode_ids[o];
			o = lzmafilter_option(L, 2, "mf", matchfinder_opts);
			filter->options.lzma.mf = matchfinder_ids[o];
			filter->options.lzma.dict_size = 
					lzmafilter_optint(L, 2, "dict_size", LZMA_DICT_SIZE_DEFAULT/1024) * 1024;
			filter->options.lzma.lc = lzmafilter_optint(L, 2, "lc", LZMA_LC_DEFAULT);
			filter->options.lzma.lp = lzmafilter_optint(L, 2, "lp", LZMA_LP_DEFAULT);
			filter->options.lzma.pb = lzmafilter_optint(L, 2, "pb", LZMA_PB_DEFAULT);
			filter->options.lzma.nice_len = lzmafilter_optint(L, 2, "nice_len", 64);
			filter->options.lzma.depth = lzmafilter_optint(L, 2, "depth", 0);
		}
		break;
	case 3:
		filter = new_lzmafilter(L, filter_ids[ftype], LZMAFILTER_DELTA_MT);
		filter->options.delta.type = LZMA_DELTA_TYPE_BYTE;
		filter->options.delta.dist = 0;
		if (str != NULL)
		{
			alloc.buf = &filter->options;
			alloc.len = sizeof(lzma_options_delta);
			status = lzma_properties_decode(&filter->head, &alloc.lzma, (const uint8_t*)str, len);
			if (status != LZMA_OK)
				luaL_argerror(L, 2, status_to_string[status]);
		}
		else if (lua_istable(L, 2))
		{
			filter->options.delta.dist = lzmafilter_optint(L, 2, "dist", 0);
		}
		break;
	case 4: case 5: case 6:
	case 7: case 8: case 9:
		filter = new_lzmafilter(L, filter_ids[ftype], LZMAFILTER_BCJ_MT);
		filter->options.bcj.start_offset = 0;
		if (str != NULL)
		{
			alloc.buf = &filter->options;
			alloc.len = sizeof(lzma_options_bcj);
			status = lzma_properties_decode(&filter->head, &alloc.lzma, (const uint8_t*)str, len);
			if (status != LZMA_OK)
				luaL_argerror(L, 2, status_to_string[status]);
		}
		else if (lua_istable(L, 2))
		{
			filter->options.bcj.start_offset = lzmafilter_optint(L, 2, "start_offset", 0);
		}
		break;
	}
	return 1;
}

static int lzmafilter_tostring(lua_State *L)
{
	lzma_ret status;
	luaL_Buffer b;
	size_t sz;
	filter_userdata *filter = get_lzmafilter(L, 1);
	luaL_buffinit(L, &b);
	status = lzma_properties_size(&sz, &filter->head);
	if (status != LZMA_OK)
		luaL_error(L, status_to_string[status]);
	if (sz > LUAL_BUFFERSIZE)
		luaL_error(L, status_to_string[LZMA_MEM_ERROR]);
	if (sz > 0)
	{
		status = lzma_properties_encode(&filter->head, (uint8_t*)luaL_prepbuffer(&b));
		if (status != LZMA_OK)
			luaL_error(L, status_to_string[status]);
		luaL_addsize(&b, sz);
	}
	luaL_pushresult(&b);
	return 1;
}

static int lzmafilter_lzma_newindex(lua_State *L)
{
	filter_userdata *filter = get_lzmafilter(L, 1);
	const char *str = luaL_checkstring(L, 2);
	if (0 == strcmp(str, "dict_size"))
		filter->options.lzma.dict_size = luaL_checknumber(L, 3) * 1024;
	else if (0 == strcmp(str, "lc"))
		filter->options.lzma.lc = luaL_checknumber(L, 3);
	else if (0 == strcmp(str, "lp"))
		filter->options.lzma.lp = luaL_checknumber(L, 3);
	else if (0 == strcmp(str, "pb"))
		filter->options.lzma.pb = luaL_checknumber(L, 3);
	else if (0 == strcmp(str, "nice_len"))
		filter->options.lzma.nice_len = luaL_checknumber(L, 3);
	else if (0 == strcmp(str, "depth"))
		filter->options.lzma.depth = luaL_checknumber(L, 3);
	else if (0 == strcmp(str, "mode"))
		filter->options.lzma.mode = lzmamode_ids[luaL_checkoption(L, 3, NULL, lzmamode_opts)];
	else if (0 == strcmp(str, "mf"))
		filter->options.lzma.mf = matchfinder_ids[luaL_checkoption(L, 3, NULL, matchfinder_opts)];
	else
		return luaL_error(L, "\"%s\" is not a valid option for this filter", str);
	return 0;
}

static int lzmafilter_lzma_index(lua_State *L)
{
	filter_userdata *filter = get_lzmafilter(L, 1);
	const char *str = luaL_checkstring(L, 2);
	if (0 == strcmp(str, "dict_size"))
		lua_pushnumber(L, filter->options.lzma.dict_size / 1024);
	else if (0 == strcmp(str, "lc"))
		lua_pushnumber(L, filter->options.lzma.lc);
	else if (0 == strcmp(str, "lp"))
		lua_pushnumber(L, filter->options.lzma.lp);
	else if (0 == strcmp(str, "pb"))
		lua_pushnumber(L, filter->options.lzma.pb);
	else if (0 == strcmp(str, "nice_len"))
		lua_pushnumber(L, filter->options.lzma.nice_len);
	else if (0 == strcmp(str, "depth"))
		lua_pushnumber(L, filter->options.lzma.depth);
	else if (0 == strcmp(str, "mode"))
		switch (filter->options.lzma.mode)
		{
		case LZMA_MODE_NORMAL:
			lua_pushliteral(L, "normal"); break;
		case LZMA_MODE_FAST:
			lua_pushliteral(L, "fast"); break;
		}
	else if (0 == strcmp(str, "mf"))
		switch (filter->options.lzma.mf)
		{
		case LZMA_MF_BT2:
			lua_pushliteral(L, "bt2"); break;
		case LZMA_MF_BT3:
			lua_pushliteral(L, "bt3"); break;
		case LZMA_MF_BT4:
			lua_pushliteral(L, "bt4"); break;
		case LZMA_MF_HC3:
			lua_pushliteral(L, "hc3"); break;
		case LZMA_MF_HC4:
			lua_pushliteral(L, "hc4"); break;
		}
	else
		lua_pushnil(L);
	return 1;
}

static int lzmafilter_delta_newindex(lua_State *L)
{
	filter_userdata *filter = get_lzmafilter(L, 1);
	const char *str = luaL_checkstring(L, 2);
	uint32_t dist;
	if (0 != strcmp(str, "dist"))
		return luaL_error(L, "\"%s\" is not a valid option for this filter", str);
	dist = luaL_checknumber(L, 3);
	filter->options.delta.dist = dist;
	return 0;
}

static int lzmafilter_delta_index(lua_State *L)
{
	filter_userdata *filter = get_lzmafilter(L, 1);
	const char *str = luaL_checkstring(L, 2);
	if (0 == strcmp(str, "dist"))
		lua_pushnumber(L, filter->options.delta.dist);
	else
		lua_pushnil(L);
	return 1;
}

static int lzmafilter_bcj_newindex(lua_State *L)
{
	filter_userdata *filter = get_lzmafilter(L, 1);
	const char *str = luaL_checkstring(L, 2);
	uint32_t startoffset;
	if (0 != strcmp(str, "start_offset"))
		return luaL_error(L, "\"%s\" is not a valid option for this filter", str);
	startoffset = luaL_checknumber(L, 3);
	filter->options.bcj.start_offset = startoffset;
	return 0;
}

static int lzmafilter_bcj_index(lua_State *L)
{
	filter_userdata *filter = get_lzmafilter(L, 1);
	const char *str = luaL_checkstring(L, 2);
	if (0 == strcmp(str, "start_offset"))
		lua_pushnumber(L, filter->options.bcj.start_offset);
	else
		lua_pushnil(L);
	return 1;
}

static int check_has_filters(lua_State *L, int n)
{
	int hasfilters = 0;
	lua_getfield(L, n, "filter");
	if (!lua_isnil(L, -1))
	{
		if (lua_objlen(L, -1) > 4)
			luaL_error(L, "too many filters");
		hasfilters = 1;
	}
	lua_pop(L, 1);
	return hasfilters;
}

static int lzmauserdata_gc(lua_State *L)
{
	z_userdata *ud = (z_userdata*)lua_touserdata(L, 1);
	lzma_end(&ud->z);
	return 0;
}

static int encode_to_buffer(lua_State *L, z_userdata *ud)
{
	luaL_Buffer B;
	luaL_buffinit(L, &B);
	do
	{
		ud->z.next_out = (uint8_t*)luaL_prepbuffer(&B);
		ud->z.avail_out = LUAL_BUFFERSIZE;
		ud->status = lzma_code(&ud->z, ud->flush);
		if (ud->status != LZMA_OK && ud->status != LZMA_STREAM_END && ud->status != LZMA_BUF_ERROR)
			break;
		luaL_addsize(&B, LUAL_BUFFERSIZE - ud->z.avail_out);
	}
	while (ud->z.avail_out == 0);
	if ((ud->status == LZMA_OK || ud->status == LZMA_STREAM_END) && ud->z.avail_in != 0)
		return luaL_error(L, "unknown failure in encode");
	luaL_pushresult(&B);
	return 1;
}

static int protected_encode_to_buffer(lua_State *L)
{
	z_userdata *ud = (z_userdata*)lua_touserdata(L, 1);
	encode_to_buffer(L, ud);
	ud->result = luaL_ref(L, LUA_REGISTRYINDEX);
	return 0;
}

static int encode_call(lua_State *L)
{
	z_userdata *ud = (z_userdata*)lua_touserdata(L, lua_upvalueindex(1));
	size_t len;
	const char *str = luaL_optlstring(L, 1, NULL, &len);
	
	if (str != NULL)
	{
		ud->z.next_in = (uint8_t*)str;
		ud->z.avail_in = len;
		ud->flush = LZMA_RUN;
	}
	else
	{
		ud->z.next_in = (uint8_t*)"";
		ud->z.avail_in = 0;
		ud->flush = LZMA_FINISH;
	}
	encode_to_buffer(L, ud);
	ud->status = status_to_errcode[ud->status];
	lua_pushinteger(L, len - ud->z.avail_in);
	lua_pushinteger(L, ud->status);
	return 3;
}

static lzma_ret encoder_init_filters(lua_State *L, lzma_stream *stream,
				int format, lzma_check check)
{
	lzma_filter filter[LZMA_FILTERS_MAX+1];
	filter_userdata *fdata;
	lzma_ret status;
	size_t numfilters;
	size_t i;
	
	if (lua_istable(L, -1))
	{
		numfilters = lua_objlen(L, -1);
		for (i=0; i<numfilters; i++)
		{
			lua_rawgeti(L, -1, i+1);
			fdata = get_lzmafilter(L, -1);
			filter[i] = fdata->head;
			lua_pop(L, 1);
		}
		filter[numfilters].id = LZMA_VLI_UNKNOWN;
	}
	else
	{
		fdata = get_lzmafilter(L, -1);
		filter[0] = fdata->head;
		filter[1].id = LZMA_VLI_UNKNOWN;
		numfilters = 1;
	}
	switch (format)
	{
	case 0: /* lzma */
		status = lzma_alone_encoder(stream, filter[0].options);
		break;
	case 1: /* xz */
		status = lzma_stream_encoder(stream, filter, check);
		break;
	case 2: /* raw */
		status = lzma_raw_encoder(stream, filter);
		break;
	}
	lua_pop(L, 1);
	return status;
}

static lzma_ret encoder_init(lua_State *L, lzma_stream *stream,
				int format, int preset, lzma_vli id, lzma_check check)
{
	lzma_filter filter[LZMA_FILTERS_MAX+1];
	lzma_options_lzma options;
	lzma_ret status = LZMA_PROG_ERROR;
	
	if (lzma_lzma_preset(&options, preset))
		return LZMA_OPTIONS_ERROR;
	
	filter[0].id = id;
	filter[0].options = &options;
	filter[1].id = LZMA_VLI_UNKNOWN;
	switch (format)
	{
	case 0: /* lzma */
		status = lzma_alone_encoder(stream, &options);
		break;
	case 1: /* xz */
		status = lzma_stream_encoder(stream, filter, check);
		break;
	case 2: /* raw */
		status = lzma_raw_encoder(stream, filter);
		break;
	}
	return status;
}

static const char * const format_opts[] = { "lzma","xz","raw",NULL };
static const char * const method_opts[] = { "lzma1","lzma2",NULL };
static const lzma_vli method_ids[] = { LZMA_FILTER_LZMA1,LZMA_FILTER_LZMA2 };
static const char * const check_opts[] = { "none","crc32","crc64","sha256",NULL };
static const lzma_check check_ids[] = { LZMA_CHECK_NONE,LZMA_CHECK_CRC32,LZMA_CHECK_CRC64,LZMA_CHECK_SHA256 };

/**
 * Compress a string.
 * options:
 *   preset=[0,9]
 *   method=lzma1|lzma2
 */
static int larc_lzma_compress(lua_State *L)
{
	int preset = LZMA_PRESET_DEFAULT,
		methid = 0,
		format = 0,
		crcid = 1,
		hasfilters = 0;
	size_t len;
	const char *str = luaL_checklstring(L, 1, &len);
	z_userdata ud = {LZMA_STREAM_INIT};

	if (lua_gettop(L) > 1)
	{
		luaL_checktype(L, 2, LUA_TTABLE);
		GETINT2OPTION(2,preset,level);
		lua_getfield(L, 2, "format");
		format = luaL_checkoption(L, -1, "lzma", format_opts);
		lua_pop(L, 1);
		lua_getfield(L, 2, "method");
		methid = luaL_checkoption(L, -1, format==1?"lzma2":"lzma1", method_opts);
		lua_pop(L, 1);
		lua_getfield(L, 2, "check");
		crcid = luaL_checkoption(L, -1, "crc32", check_opts);
		lua_pop(L, 1);
		hasfilters = check_has_filters(L, 2);
	}
	
	if (hasfilters)
	{
		lua_getfield(L, 2, "filter");
		ud.status = encoder_init_filters(L, &ud.z, format, check_ids[crcid]);
	}
	else
		ud.status = encoder_init(L, &ud.z, format, preset, method_ids[methid], check_ids[crcid]);
	if (ud.status != LZMA_OK)
	{
		lua_pushnil(L);
		lua_pushstring(L, status_to_string[ud.status]);
		lua_pushinteger(L, status_to_errcode[ud.status]);
		return 3;
	}
	
	ud.z.next_in = (uint8_t *)str;
	ud.z.avail_in = len;
	ud.result = -1;
	ud.flush = LZMA_FINISH;
	if (0 != lua_cpcall(L, protected_encode_to_buffer, &ud))
	{
		lzma_end(&ud.z);
		return lua_error(L);
	}
	lzma_end(&ud.z);
	
	if (ud.result != -1)
	{
		lua_rawgeti(L, LUA_REGISTRYINDEX, ud.result);
		luaL_unref(L, LUA_REGISTRYINDEX, ud.result);
		lua_pushinteger(L, len - ud.z.avail_in);
	}
	else
	{
		lua_pushnil(L);
		lua_pushstring(L, status_to_string[ud.status]);
	}
	lua_pushinteger(L, status_to_errcode[ud.status]);
	return 3;
}

/**
 * Create a compress function.
 * options:
 *   preset=[0,9]
 */
static int larc_lzma_compressor(lua_State *L)
{
	int preset = LZMA_PRESET_DEFAULT,
		methid = 0,
		format = 0,
		crcid = 1,
		hasfilters = 0;
	z_userdata *ud;

	if (lua_gettop(L) > 0)
	{
		luaL_checktype(L, 1, LUA_TTABLE);
		GETINT2OPTION(1,preset,level);
		lua_getfield(L, 1, "format");
		format = luaL_checkoption(L, -1, "lzma", format_opts);
		lua_pop(L, 1);
		lua_getfield(L, 1, "method");
		methid = luaL_checkoption(L, -1, format==1?"lzma2":"lzma1", method_opts);
		lua_pop(L, 1);
		lua_getfield(L, 1, "check");
		crcid = luaL_checkoption(L, -1, "crc32", check_opts);
		lua_pop(L, 1);
		hasfilters = check_has_filters(L, 1);
	}
	
	ud = (z_userdata*)lua_newuserdata(L, sizeof(z_userdata));
	luaL_getmetatable(L, LZMA_MT);
	lua_setmetatable(L, -2);
	memset(&ud->z, 0, sizeof(lzma_stream));
	
	ud->status = encoder_init(L, &ud->z, format, preset, method_ids[methid], check_ids[crcid]);
	if (ud->status != LZMA_OK)
	{
		lua_pushnil(L);
		lua_pushstring(L, status_to_string[ud->status]);
		lua_pushinteger(L, status_to_errcode[ud->status]);
		return 3;
	}
	
	lua_pushcclosure(L, encode_call, 1);
	return 1;
}

static int decode_to_buffer(lua_State *L, z_userdata *ud)
{
	luaL_Buffer B;
	luaL_buffinit(L, &B);
	do
	{
		ud->z.next_out = (unsigned char*)luaL_prepbuffer(&B);
		ud->z.avail_out = LUAL_BUFFERSIZE;
		ud->status = lzma_code(&ud->z, LZMA_RUN);
		if (ud->status != LZMA_OK && ud->status != LZMA_STREAM_END && ud->status != LZMA_BUF_ERROR)
			break;
		luaL_addsize(&B, LUAL_BUFFERSIZE - ud->z.avail_out);
	}
	while (ud->z.avail_out == 0);
	if (ud->status == LZMA_OK && ud->z.avail_in != 0)
		return luaL_error(L, "unknown failure in decode");
	luaL_pushresult(&B);
	return 1;
}

static int protected_decode_to_buffer(lua_State *L)
{
	z_userdata *ud = (z_userdata*)lua_touserdata(L, 1);
	decode_to_buffer(L, ud);
	ud->result = luaL_ref(L, LUA_REGISTRYINDEX);
	return 0;
}

static int decode_call(lua_State *L)
{
	z_userdata *ud = (z_userdata*)lua_touserdata(L, lua_upvalueindex(1));
	size_t len;
	const char *str = luaL_optlstring(L, 1, NULL, &len);
	
	if (len > 0)
	{
		ud->z.next_in = (unsigned char*)str;
		ud->z.avail_in = len;
		decode_to_buffer(L, ud);
		ud->status = status_to_errcode[ud->status];
		lua_pushinteger(L, len - ud->z.avail_in);
		lua_pushinteger(L, ud->status);
	}
	else
	{
		lua_pushliteral(L, "");
		lua_pushinteger(L, 0);
		lua_pushinteger(L, 0);
	}
	return 3;
}

static lzma_ret decoder_init_filters(lua_State *L, lzma_stream *stream)
{
	lzma_filter filter[LZMA_FILTERS_MAX+1];
	filter_userdata *fdata;
	lzma_ret status;
	size_t numfilters;
	size_t i;
	
	if (lua_istable(L, -1))
	{
		numfilters = lua_objlen(L, -1);
		for (i=0; i<numfilters; i++)
		{
			lua_rawgeti(L, -1, i+1);
			fdata = get_lzmafilter(L, -1);
			filter[i] = fdata->head;
			lua_pop(L, 1);
		}
		filter[numfilters].id = LZMA_VLI_UNKNOWN;
	}
	else
	{
		fdata = get_lzmafilter(L, -1);
		filter[0] = fdata->head;
		filter[1].id = LZMA_VLI_UNKNOWN;
		numfilters = 1;
	}
	status = lzma_raw_decoder(stream, filter);
	lua_pop(L, 1);
	return status;
}

static lzma_ret decoder_init(lua_State *L, lzma_stream *stream, int format, lzma_vli id)
{
	uint64_t mem;
	lzma_ret status = LZMA_PROG_ERROR;
	
	switch (format)
	{
	case 0: /* lzma */
		mem = lzma_physmem() / 2;
		if (mem == 0) /* make a guess */
			mem = 32ULL * 1024 * 1024;
		status = lzma_alone_decoder(stream, mem);
		break;
	case 1: /* xz */
		mem = lzma_physmem() / 2;
		if (mem == 0) /* make a guess */
			mem = 32ULL * 1024 * 1024;
		status = lzma_stream_decoder(stream, mem, 0);
		break;
	}
	return status;
}

/**
 * Decompress a string.
 * Returns a string,number,number when successful.
 * Returns nil,string,number if there is an error.
 * options:
 *   method=lzma1|lzma2
 */
static int larc_lzma_decompress(lua_State *L)
{
	int methid = 0,
		format = 0,
		hasfilters = 0;
	size_t len;
	const char *str = luaL_checklstring(L, 1, &len);
	z_userdata ud = {LZMA_STREAM_INIT};

	if (lua_gettop(L) > 1)
	{
		luaL_checktype(L, 2, LUA_TTABLE);
		lua_getfield(L, 2, "format");
		format = luaL_checkoption(L, -1, "lzma", format_opts);
		lua_pop(L, 1);
		lua_getfield(L, 2, "method");
		methid = luaL_checkoption(L, -1, format==1?"lzma2":"lzma1", method_opts);
		lua_pop(L, 1);
		hasfilters = check_has_filters(L, 2);
	}
	
	if (format == 2)
	{
		if (!hasfilters)
			luaL_error(L, "raw decompress requires filters");
		lua_getfield(L, 2, "filter");
		ud.status = decoder_init_filters(L, &ud.z);
	}
	else
		ud.status = decoder_init(L, &ud.z, format, method_ids[methid]);
	if (ud.status != LZMA_OK)
	{
		lua_pushnil(L);
		lua_pushstring(L, status_to_string[ud.status]);
		lua_pushinteger(L, status_to_errcode[ud.status]);
		return 3;
	}
	
	ud.z.next_in = (uint8_t *)str;
	ud.z.avail_in = len;
	ud.result = -1;
	if (0 != lua_cpcall(L, protected_decode_to_buffer, &ud))
	{
		lzma_end(&ud.z);
		return lua_error(L);
	}
	lzma_end(&ud.z);
	
	if (ud.result != -1)
	{
		lua_rawgeti(L, LUA_REGISTRYINDEX, ud.result);
		luaL_unref(L, LUA_REGISTRYINDEX, ud.result);
		lua_pushinteger(L, len - ud.z.avail_in);
	}
	else
	{
		lua_pushnil(L);
		lua_pushstring(L, status_to_string[ud.status]);
	}
	lua_pushinteger(L, status_to_errcode[ud.status]);
	return 3;
}

/**
 * Create an decompress function.
 * Returns a function when successful.
 * Returns nil,string,number if there is an error.
 * options:
 */
static int larc_lzma_decompressor(lua_State *L)
{
	int methid = 0,
		format = 0,
		hasfilters = 0;
	z_userdata *ud;
	
	if (lua_gettop(L) > 0)
	{
		luaL_checktype(L, 1, LUA_TTABLE);
		lua_getfield(L, 1, "format");
		format = luaL_checkoption(L, -1, "lzma", format_opts);
		lua_pop(L, 1);
		lua_getfield(L, 1, "method");
		methid = luaL_checkoption(L, -1, format==1?"lzma2":"lzma1", method_opts);
		lua_pop(L, 1);
		hasfilters = check_has_filters(L, 1);
	}

	ud = (z_userdata*)lua_newuserdata(L, sizeof(z_userdata));
	luaL_getmetatable(L, LZMA_MT);
	lua_setmetatable(L, -2);
	memset(&ud->z, 0, sizeof(lzma_stream));
	
	if (format == 2)
	{
		if (!hasfilters)
			luaL_error(L, "raw decompress requires filters");
		lua_getfield(L, 1, "filter");
		ud->status = decoder_init_filters(L, &ud->z);
	}
	else
		ud->status = decoder_init(L, &ud->z, format, method_ids[methid]);
	if (ud->status != LZMA_OK)
	{
		lua_pushnil(L);
		lua_pushstring(L, status_to_string[ud->status]);
		lua_pushinteger(L, status_to_errcode[ud->status]);
		return 3;
	}
	
	lua_pushcclosure(L, decode_call, 1);
	return 1;
}

/**
 * Compute the CRC32 hash of a string.
 */
static int larc_lzma_crc32(lua_State *L)
{
	uint32_t crc;
	size_t n;
	const char *s;
	
	if (lua_gettop(L) == 0)
	{
		lua_pushnumber(L, lzma_crc32(NULL, 0, 0));
		return 1;
	}
	if (lua_gettop(L) < 2 || lua_isnil(L, 1))
	{
		crc = lzma_crc32(NULL, 0, 0);
		s = luaL_optlstring(L, -1, NULL, &n);
	}
	else
	{
		crc = luaL_checknumber(L, 1);
		s = luaL_checklstring(L, 2, &n);
	}
	if (n > 0)
		crc = lzma_crc32((const uint8_t*)s, n, crc);
	lua_pushnumber(L, crc);
	return 1;
}

/**
 * Compute the CRC64 hash of a string.
 */
static int larc_lzma_crc64(lua_State *L)
{
	uint64_t crc;
	size_t n;
	const char *s;
	
	if (lua_gettop(L) == 0)
	{
		newuint64(L, lzma_crc64(NULL, 0, 0));
		return 1;
	}
	if (lua_gettop(L) < 2 || lua_isnil(L, 1))
	{
		crc = lzma_crc64(NULL, 0, 0);
		s = luaL_optlstring(L, -1, NULL, &n);
	}
	else
	{
		crc = getuint64(L, 1);
		s = luaL_checklstring(L, 2, &n);
	}
	if (n > 0)
		crc = lzma_crc64((const uint8_t*)s, n, crc);
	newuint64(L, crc);
	return 1;
}

/**
 * Get what LZMA thinks is the maximum memory available.
 */
static int larc_lzma_physmem(lua_State *L)
{
	uint64_t kbytes = lzma_physmem();
	unsigned long bytes = kbytes % 1024;
	double kibytes = (double)(kbytes/1024) + ((double)bytes/1024.0);
	lua_pushnumber(L, kibytes);
	return 1;
}


#ifdef _WIN32
#undef LUAMOD_API
#define LUAMOD_API      __declspec(dllexport)
#endif

static const luaL_Reg lzmafilter_lzma_mt[] = 
{
	{"__index", lzmafilter_lzma_index},
	{"__newindex", lzmafilter_lzma_newindex},
	{"__tostring", lzmafilter_tostring},
	{NULL, NULL}
};
static const luaL_Reg lzmafilter_delta_mt[] = 
{
	{"__index", lzmafilter_delta_index},
	{"__newindex", lzmafilter_delta_newindex},
	{"__tostring", lzmafilter_tostring},
	{NULL, NULL}
};
static const luaL_Reg lzmafilter_bcj_mt[] = 
{
	{"__index", lzmafilter_bcj_index},
	{"__newindex", lzmafilter_bcj_newindex},
	{"__tostring", lzmafilter_tostring},
	{NULL, NULL}
};

static const luaL_Reg larc_lzma_Reg[] = 
{
	{"compress", larc_lzma_compress},
	{"decompress", larc_lzma_decompress},
	{"compressor", larc_lzma_compressor},
	{"decompressor", larc_lzma_decompressor},
	{"filter", larc_lzmafilter_new},
	{"crc32", larc_lzma_crc32},
	{"crc64", larc_lzma_crc64},
	{"physmem", larc_lzma_physmem},
	{NULL, NULL}
};

LUAMOD_API int luaopen_larc_lzma(lua_State *L)
{
	lua_getglobal(L, "require");
	lua_pushliteral(L, "larc.struct");
	lua_call(L, 1, 0);
	luaL_newmetatable(L, LZMAFILTER_LZMA_MT);
	luaL_register(L, NULL, lzmafilter_lzma_mt);
	lua_pop(L, 1);
	luaL_newmetatable(L, LZMAFILTER_DELTA_MT);
	luaL_register(L, NULL, lzmafilter_delta_mt);
	lua_pop(L, 1);
	luaL_newmetatable(L, LZMAFILTER_BCJ_MT);
	luaL_register(L, NULL, lzmafilter_bcj_mt);
	lua_pop(L, 1);
	luaL_newmetatable(L, LZMA_MT);
	lua_pushcfunction(L, lzmauserdata_gc);
	lua_setfield(L, -2, "__gc");
	lua_pop(L, 1);
	luaL_register(L, "larc.lzma", larc_lzma_Reg);
	lua_pushstring(L, lzma_version_string());
	lua_setfield(L, -2, "LZMA_VERSION");
	SETCONSTANT(LZMA_OK);
	SETCONSTANT(LZMA_STREAM_END);
	SETCONSTANT(LZMA_NO_CHECK);
	SETCONSTANT(LZMA_UNSUPPORTED_CHECK);
	SETCONSTANT(LZMA_GET_CHECK);
	SETERRORCONSTANT(LZMA_MEM_ERROR);
	SETERRORCONSTANT(LZMA_MEMLIMIT_ERROR);
	SETERRORCONSTANT(LZMA_FORMAT_ERROR);
	SETERRORCONSTANT(LZMA_OPTIONS_ERROR);
	SETERRORCONSTANT(LZMA_DATA_ERROR);
	SETERRORCONSTANT(LZMA_BUF_ERROR);
	SETERRORCONSTANT(LZMA_PROG_ERROR);
	return 1;
}
