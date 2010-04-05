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
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "lzma.h"

#define LZMA_MT	"larc.lzma.stream"

#define GETINTOPTION(arg,opt)	{ \
		lua_getfield(L, arg, #opt); \
		opt = luaL_optint(L, -1, opt); \
		lua_pop(L, 1); }
#define SETCONSTANT(c)	{ \
		lua_pushinteger(L, c); \
		lua_setfield(L, -2, #c); }
#define SETERRORCONSTANT(c)	{ \
		lua_pushinteger(L, -c); \
		lua_setfield(L, -2, #c); }

#if LUA_VERSION_NUM > 501
static int lua_cpcall(lua_State *L, lua_CFunction func, void *ud)
{
	lua_CFunction pfunc = func;
	lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_CPCALL);
	lua_pushlightuserdata(L, &pfunc);
	lua_pushlightuserdata(L, ud);
	return lua_pcall(L, 2, 0, 0);
}
#endif

#define UINT64TYPE	"large integer"

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

typedef struct lzma_userdata
{
	lzma_stream z;
	lzma_ret status;
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
	if (!lua_isnil(L, lua_upvalueindex(2)))
	{
		lua_pushvalue(L, lua_upvalueindex(2));
		lua_insert(L, -2);
		lua_concat(L, 2);
		lua_pushnil(L);
		lua_replace(L, lua_upvalueindex(2));
	}
	lua_pushinteger(L, len - ud->z.avail_in);
	lua_pushinteger(L, ud->status);
	return 3;
}

static lzma_ret encoder_init(lua_State *L, lzma_stream *stream, lzma_vli id, int preset)
{
	uint8_t propbuf[32]; /* hooray variable length data formats */
	size_t propsize;
	lzma_filter filter[LZMA_FILTERS_MAX+1];
	lzma_options_lzma options;
	lzma_ret status;
	
	if (lzma_lzma_preset(&options, preset))
		return LZMA_OPTIONS_ERROR;
	
	filter[0].id = id;
	filter[0].options = &options;
	filter[1].id = LZMA_VLI_UNKNOWN;
	status = lzma_raw_encoder(stream, filter);
	if (status != LZMA_OK)
		return status;
	lzma_properties_size(&propsize, filter);
	lzma_properties_encode(filter, propbuf);
	lua_pushlstring(L, (char*)propbuf, propsize);
	return LZMA_OK;
}

/**
 * Compress a string.
 * options:
 *   level=[0,9]
 *   method=lzma1|lzma2
 */
static int larc_lzma_compress(lua_State *L)
{
	int level = LZMA_PRESET_DEFAULT;
	size_t len;
	const char *str = luaL_checklstring(L, 1, &len);
	z_userdata ud = {LZMA_STREAM_INIT};

	if (lua_gettop(L) > 1)
	{
		luaL_checktype(L, 2, LUA_TTABLE);
		GETINTOPTION(2,level);
	}
	
	ud.status = encoder_init(L, &ud.z, LZMA_FILTER_LZMA2, level);
	switch (ud.status)
	{
	case LZMA_OK:
		break;
	case LZMA_PROG_ERROR:
	case LZMA_OPTIONS_ERROR:
		return luaL_argerror(L, 2, "invalid preset");
	default:
		lua_pushnil(L);
		lua_pushstring(L, ""); /* TODO error message */
		lua_pushinteger(L, ud.status);
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
		/* add filter properties header */
		lua_concat(L, 2);
		lua_pushinteger(L, len - ud.z.avail_in);
	}
	else
	{
		lua_pushnil(L);
		lua_pushstring(L, ""); /* TODO error message */
	}
	lua_pushinteger(L, ud.status);
	return 3;
}

/**
 * Create a compress function.
 * options:
 *   level=[0,9]
 */
static int larc_lzma_compressor(lua_State *L)
{
	int level = LZMA_PRESET_DEFAULT;
	z_userdata *ud;

	if (lua_gettop(L) > 0)
	{
		luaL_checktype(L, 1, LUA_TTABLE);
		GETINTOPTION(1,level);
	}
	
	ud = (z_userdata*)lua_newuserdata(L, sizeof(z_userdata));
	luaL_getmetatable(L, LZMA_MT);
	lua_setmetatable(L, -2);
	memset(&ud->z, 0, sizeof(lzma_stream));
	
	ud->status = encoder_init(L, &ud->z, LZMA_FILTER_LZMA2, level);
	switch (ud->status)
	{
	case LZMA_OK:
		break;
	case LZMA_PROG_ERROR:
	case LZMA_OPTIONS_ERROR:
		return luaL_argerror(L, 2, "invalid preset");
	default:
		lua_pushnil(L);
		lua_pushstring(L, ""); /* TODO error message */
		lua_pushinteger(L, ud->status);
		return 3;
	}
	
	lua_pushcclosure(L, encode_call, 2);
	return 1;
}

static int decode_to_buffer(lua_State *L, z_userdata *ud)
{
	luaL_Buffer B;
	luaL_buffinit(L, &B);
/*
	do
	{
		ud->z.next_out = (unsigned char*)luaL_prepbuffer(&B);
		ud->z.avail_out = LUAL_BUFFERSIZE;
		ud->status = inflate(&ud->z, Z_NO_FLUSH);
		if (ud->status != Z_OK && ud->status != Z_STREAM_END)
			break;
		luaL_addsize(&B, LUAL_BUFFERSIZE - ud->z.avail_out);
	}
	while (ud->z.avail_out == 0);
	if (ud->status == Z_OK && ud->z.avail_in != 0)
		return luaL_error(L, "unknown failure in inflate");
*/
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
/*
	if (len > 0)
	{
		ud->z.next_in = (unsigned char*)str;
		ud->z.avail_in = len;
		decode_to_buffer(L, ud);
		lua_pushinteger(L, len - ud->z.avail_in);
		lua_pushinteger(L, ud->status);
	}
	else
*/
	{
		lua_pushliteral(L, "");
		lua_pushinteger(L, 0);
		lua_pushinteger(L, 0);
	}
	return 3;
}

/**
 * Inflate a string.
 * Returns a string,number,number when successful.
 * Returns nil,string,number if there is an error.
 * options:
 */
static int larc_lzma_decompress(lua_State *L)
{
	int wbits = 15;
	size_t len;
	const char *str = luaL_checklstring(L, 1, &len);
	z_userdata ud;

/*
	if (lua_gettop(L) > 1)
	{
		luaL_checktype(L, 2, LUA_TTABLE);
		GETINTOPTION(2,wbits);
	}
	
	ud.z.zalloc = Z_NULL;
	ud.z.zfree = Z_NULL;
	ud.z.opaque = Z_NULL;
	ud.z.next_in = (unsigned char*)str;
	ud.z.avail_in = len;
	
	ud.status = inflateInit2(&ud.z, wbits);
	if (ud.status != Z_OK)
	{
		lua_pushnil(L);
		lua_pushstring(L, zError(ud.status));
		lua_pushinteger(L, ud.status);
		return 3;
	}
	
	ud.result = -1;
	if (0 != lua_cpcall(L, protected_decode_to_buffer, &ud))
	{
		inflateEnd(&ud.z);
		return lua_error(L);
	}
	inflateEnd(&ud.z);
	
	if (ud.result != -1)
	{
		lua_rawgeti(L, LUA_REGISTRYINDEX, ud.result);
		luaL_unref(L, LUA_REGISTRYINDEX, ud.result);
		lua_pushinteger(L, len - ud.z.avail_in);
	}
	else
	{
		lua_pushnil(L);
		lua_pushstring(L, zError(ud.status));
	}
	lua_pushinteger(L, ud.status);
	return 3;
*/
	return 0;
}

/**
 * Create an decompress function.
 * Returns a function when successful.
 * Returns nil,string,number if there is an error.
 * options:
 */
static int larc_lzma_decompressor(lua_State *L)
{
	int wbits = 15;
	z_userdata *ud;
/*
	if (lua_gettop(L) > 0)
	{
		luaL_checktype(L, 1, LUA_TTABLE);
		GETINTOPTION(1,wbits);
	}

	ud = (z_userdata*)lua_newuserdata(L, sizeof(z_userdata));
	luaL_getmetatable(L, INFLATE_MT);
	lua_setmetatable(L, -2);
	
	ud->z.zalloc = Z_NULL;
	ud->z.zfree = Z_NULL;
	ud->z.opaque = Z_NULL;
	ud->z.next_in = Z_NULL;
	ud->z.avail_in = 0;
	
	ud->status = inflateInit2(&ud->z, wbits);
	if (ud->status != Z_OK)
	{
		lua_pushnil(L);
		lua_pushstring(L, zError(ud->status));
		lua_pushinteger(L, ud->status);
		return 3;
	}

	lua_pushcclosure(L, decode_call, 1);
	return 1;
*/
	return 0;
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
	uint64_t bytes = lzma_physmem();
	lldiv_t dm = lldiv(bytes, 1024);
	double kibytes = (double)dm.rem/1024.0 + dm.quot;
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
