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

lzma.filter("delta",{distance=512}, "lzma2",{dictsize=10240}) -> filter object
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

static int lzma_userdata_gc(lua_State *L)
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

static lzma_ret encoder_init(lua_State *L, lzma_stream *stream,
				int format, int preset, lzma_vli id, lzma_check check)
{
	lzma_filter filter[LZMA_FILTERS_MAX+1];
	lzma_options_lzma options;
	lzma_ret status;
	
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
		crcid = 1;
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
	}
	
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
		crcid = 1;
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

static lzma_ret decoder_init(lua_State *L, lzma_stream *stream, int format, lzma_vli id)
{
	lzma_filter filter[LZMA_FILTERS_MAX+1];
	lzma_options_lzma options;
	uint64_t mem;
	lzma_ret status;
	
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
	case 2: /* raw */
		lzma_lzma_preset(&options, 6);
		filter[0].id = id;
		filter[0].options = &options;
		filter[1].id = LZMA_VLI_UNKNOWN;
		status = lzma_raw_decoder(stream, filter);
		break;
	}
	return status;
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
		format = 0;
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
	}
	
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
		format = 0;
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
	}

	ud = (z_userdata*)lua_newuserdata(L, sizeof(z_userdata));
	luaL_getmetatable(L, LZMA_MT);
	lua_setmetatable(L, -2);
	memset(&ud->z, 0, sizeof(lzma_stream));
	
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

static const luaL_Reg larc_lzma_Reg[] = 
{
	{"compress", larc_lzma_compress},
	{"decompress", larc_lzma_decompress},
	{"compressor", larc_lzma_compressor},
	{"decompressor", larc_lzma_decompressor},
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
	luaL_newmetatable(L, LZMA_MT);
	lua_pushcfunction(L, lzma_userdata_gc);
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
