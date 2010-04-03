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

#include "lua.h"
#include "lauxlib.h"

#include "zlib.h"

#define DEFLATE_MT	"larc.zlib.deflate"
#define INFLATE_MT	"larc.zlib.inflate"

#define GETINTOPTION(arg,opt)	{ \
		lua_getfield(L, arg, #opt); \
		opt = luaL_optint(L, -1, opt); \
		lua_pop(L, 1); }
#define SETCONSTANT(c)	{ \
		lua_pushinteger(L, c); \
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

typedef struct zlib_userdata
{
	z_stream z;
	int status;
	int result;
	int flush;
} z_userdata;

static int deflate_userdata_gc(lua_State *L)
{
	z_userdata *ud = (z_userdata*)lua_touserdata(L, 1);
	deflateEnd(&ud->z);
	return 0;
}

static int inflate_userdata_gc(lua_State *L)
{
	z_userdata *ud = (z_userdata*)lua_touserdata(L, 1);
	inflateEnd(&ud->z);
	return 0;
}

static int deflate_to_buffer(lua_State *L, z_userdata *ud)
{
	luaL_Buffer B;
	luaL_buffinit(L, &B);
	do
	{
		ud->z.next_out = (unsigned char*)luaL_prepbuffer(&B);
		ud->z.avail_out = LUAL_BUFFERSIZE;
		if ((ud->status = deflate(&ud->z, ud->flush)) == Z_STREAM_ERROR)
			break;
		luaL_addsize(&B, LUAL_BUFFERSIZE - ud->z.avail_out);
	}
	while (ud->z.avail_out == 0);
	if ((ud->status == Z_OK || ud->status == Z_STREAM_END) && ud->z.avail_in != 0)
		return luaL_error(L, "unknown failure in deflate");
	luaL_pushresult(&B);
	return 1;
}

static int protected_deflate_to_buffer(lua_State *L)
{
	z_userdata *ud = (z_userdata*)lua_touserdata(L, 1);
	deflate_to_buffer(L, ud);
	ud->result = luaL_ref(L, LUA_REGISTRYINDEX);
	return 0;
}

static int deflate_call(lua_State *L)
{
	z_userdata *ud = (z_userdata*)lua_touserdata(L, lua_upvalueindex(1));
	size_t len;
	const char *str = luaL_optlstring(L, 1, NULL, &len);
	if (str != NULL)
	{
		ud->z.next_in = (unsigned char*)str;
		ud->z.avail_in = len;
		ud->flush = Z_NO_FLUSH;
	}
	else
	{
		ud->z.next_in = (unsigned char*)"";
		ud->z.avail_in = 0;
		ud->flush = Z_FINISH;
	}
	deflate_to_buffer(L, ud);
	lua_pushinteger(L, len - ud->z.avail_in);
	lua_pushinteger(L, ud->status);
	return 3;
}

static const char *const strategy_opts[] =
	{"default","filtered","huffmanonly","rle","fixed",NULL};

/**
 * Deflate a string.
 * options:
 *   level=[0,9]
 *   wbits=[8,15]
 *   mem=[1,9]
 *   strategy=[filtered|huffmanonly|rle|fixed]
 */
static int larc_zlib_compress(lua_State *L)
{
	int level = Z_DEFAULT_COMPRESSION,
		wbits = 15,
		mem = 8,
		strategy = Z_DEFAULT_STRATEGY;
	size_t len;
	const char *str = luaL_checklstring(L, 1, &len);
	z_userdata ud;

	if (lua_gettop(L) > 1)
	{
		luaL_checktype(L, 2, LUA_TTABLE);
		GETINTOPTION(2,level);
		GETINTOPTION(2,wbits);
		GETINTOPTION(2,level);
		lua_getfield(L, 2, "strategy");
		strategy = luaL_checkoption(L, -1, "default", strategy_opts);
		lua_pop(L, 1);
	}
	
	ud.z.zalloc = Z_NULL;
	ud.z.zfree = Z_NULL;
	ud.z.opaque = Z_NULL;
	
	ud.status = deflateInit2(&ud.z, level, Z_DEFLATED, wbits, mem, strategy);
	if (ud.status != Z_OK)
	{
		lua_pushnil(L);
		lua_pushstring(L, zError(ud.status));
		lua_pushinteger(L, ud.status);
		return 3;
	}
	
	ud.z.next_in = (unsigned char *)str;
	ud.z.avail_in = len;
	ud.result = -1;
	ud.flush = Z_FINISH;
	if (0 != lua_cpcall(L, protected_deflate_to_buffer, &ud))
	{
		deflateEnd(&ud.z);
		return lua_error(L);
	}
	deflateEnd(&ud.z);
	
	if (ud.result != -1)
	{
		lua_rawgeti(L, LUA_REGISTRYINDEX, ud.result);
		luaL_unref(L, LUA_REGISTRYINDEX, ud.result);
		/* number of bytes used */
		lua_pushinteger(L, len - ud.z.avail_in);
	}
	else
	{
		lua_pushnil(L);
		lua_pushstring(L, zError(ud.status));
	}
	lua_pushinteger(L, ud.status);
	return 3;
}

/**
 * Create a deflate function.
 * options:
 *   level=[0,9]
 *   wbits=[8,15]
 *   mem=[1,9]
 *   strategy=[filtered|huffmanonly|rle|fixed]
 */
static int larc_zlib_compressor(lua_State *L)
{
	int level = Z_DEFAULT_COMPRESSION,
		wbits = 15,
		mem = 8,
		strategy = Z_DEFAULT_STRATEGY;
	z_userdata *ud;

	if (lua_gettop(L) > 0)
	{
		luaL_checktype(L, 1, LUA_TTABLE);
		GETINTOPTION(1,level);
		GETINTOPTION(1,wbits);
		GETINTOPTION(1,level);
		lua_getfield(L, 1, "strategy");
		strategy = luaL_checkoption(L, -1, "default", strategy_opts);
		lua_pop(L, 1);
	}
	
	ud = (z_userdata*)lua_newuserdata(L, sizeof(z_userdata));
	luaL_getmetatable(L, DEFLATE_MT);
	lua_setmetatable(L, -2);
	
	ud->z.zalloc = Z_NULL;
	ud->z.zfree = Z_NULL;
	ud->z.opaque = Z_NULL;
	
	ud->status = deflateInit2(&ud->z, level, Z_DEFLATED, wbits, mem, strategy);
	if (ud->status != Z_OK)
	{
		lua_pushnil(L);
		lua_pushstring(L, zError(ud->status));
		lua_pushinteger(L, ud->status);
		return 3;
	}
	
	lua_pushcclosure(L, deflate_call, 1);
	return 1;
}

static int inflate_to_buffer(lua_State *L, z_userdata *ud)
{
	luaL_Buffer B;
	luaL_buffinit(L, &B);
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
	/* not an error
	if (ud->status == Z_STREAM_END && ud->z.avail_in != 0)
		return luaL_error(L, "unhandled trailing data in inflate stream");
	*/
	luaL_pushresult(&B);
	return 1;
}

static int protected_inflate_to_buffer(lua_State *L)
{
	z_userdata *ud = (z_userdata*)lua_touserdata(L, 1);
	inflate_to_buffer(L, ud);
	ud->result = luaL_ref(L, LUA_REGISTRYINDEX);
	return 0;
}

static int inflate_call(lua_State *L)
{
	z_userdata *ud = (z_userdata*)lua_touserdata(L, lua_upvalueindex(1));
	size_t len;
	const char *str = luaL_optlstring(L, 1, NULL, &len);
	if (len > 0)
	{
		ud->z.next_in = (unsigned char*)str;
		ud->z.avail_in = len;
		inflate_to_buffer(L, ud);
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
 * Inflate a string.
 * Returns a string,number,number when successful.
 * Returns nil,string,number if there is an error.
 * options:
 *   wbits=[8,15]
 */
static int larc_zlib_decompress(lua_State *L)
{
	int wbits = 15;
	size_t len;
	const char *str = luaL_checklstring(L, 1, &len);
	z_userdata ud;

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
	if (0 != lua_cpcall(L, protected_inflate_to_buffer, &ud))
	{
		inflateEnd(&ud.z);
		return lua_error(L);
	}
	inflateEnd(&ud.z);
	
	if (ud.result != -1)
	{
		lua_rawgeti(L, LUA_REGISTRYINDEX, ud.result);
		luaL_unref(L, LUA_REGISTRYINDEX, ud.result);
		/* number of bytes used */
		lua_pushinteger(L, len - ud.z.avail_in);
	}
	else
	{
		lua_pushnil(L);
		lua_pushstring(L, zError(ud.status));
	}
	lua_pushinteger(L, ud.status);
	return 3;
}

/**
 * Create an inflate function.
 * Returns a function when successful.
 * Returns nil,string,number if there is an error.
 * options:
 *   wbits=[8,15]
 */
static int larc_zlib_decompressor(lua_State *L)
{
	int wbits = 15;
	z_userdata *ud;
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

	lua_pushcclosure(L, inflate_call, 1);
	return 1;
}

/**
 * Compute the CRC32 hash of a string.
 */
static int larc_zlib_crc32(lua_State *L)
{
	unsigned long crc;
	size_t n;
	const char *s;
	
	if (lua_gettop(L) == 0)
	{
		lua_pushnumber(L, crc32(0, NULL, 0));
		return 1;
	}
	if (lua_gettop(L) < 2 || lua_isnil(L, 1))
	{
		crc = crc32(0, NULL, 0);
		s = luaL_optlstring(L, -1, NULL, &n);
	}
	else
	{
		crc = luaL_checknumber(L, 1);
		s = luaL_checklstring(L, 2, &n);
	}
	if (n > 0)
		crc = crc32(crc, (const unsigned char*)s, n);
	lua_pushnumber(L, crc);
	return 1;
}

/**
 * Compute the sum of two CRC32 hashes given the length 
 * of the string that the second hash came from.
 */
static int larc_zlib_crc32combine(lua_State *L)
{
	unsigned long crc1, crc2;
	size_t len;
	
	crc1 = luaL_checknumber(L, 1);
	crc2 = luaL_checknumber(L, 2);
	len = luaL_checknumber(L, 3);
	lua_pushnumber(L, crc32_combine(crc1,crc2,len));
	return 1;
}

/**
 * Compute the Adler32 hash of a string.
 */
static int larc_zlib_adler32(lua_State *L)
{
	unsigned long crc;
	size_t n;
	const char *s;
	
	if (lua_gettop(L) == 0)
	{
		lua_pushnumber(L, adler32(0, NULL, 0));
		return 1;
	}
	if (lua_gettop(L) < 2 || lua_isnil(L, 1))
	{
		crc = adler32(0, NULL, 0);
		s = luaL_optlstring(L, -1, NULL, &n);
	}
	else
	{
		crc = luaL_checknumber(L, 1);
		s = luaL_checklstring(L, 2, &n);
	}
	if (n > 0)
		crc = adler32(crc, (const unsigned char*)s, n);
	lua_pushnumber(L, crc);
	return 1;
}

/**
 * Compute the sum of two Adler32 hashes given the length 
 * of the string that the second hash came from.
 */
static int larc_zlib_adler32combine(lua_State *L)
{
	unsigned long crc1, crc2;
	size_t len;
	
	crc1 = luaL_checknumber(L, 1);
	crc2 = luaL_checknumber(L, 2);
	len = luaL_checknumber(L, 3);
	lua_pushnumber(L, adler32_combine(crc1,crc2,len));
	return 1;
}


#ifdef _WIN32
#undef LUAMOD_API
#define LUAMOD_API      __declspec(dllexport)
#endif

static const luaL_Reg larc_zlib_Reg[] = 
{
	{"compress", larc_zlib_compress},
	{"decompress", larc_zlib_decompress},
	{"compressor", larc_zlib_compressor},
	{"decompressor", larc_zlib_decompressor},
	{"crc32", larc_zlib_crc32},
	{"crc32_combine", larc_zlib_crc32combine},
	{"adler32", larc_zlib_adler32},
	{"adler32_combine", larc_zlib_adler32combine},
	{NULL, NULL}
};

LUAMOD_API int luaopen_larc_zlib(lua_State *L)
{
	luaL_newmetatable(L, DEFLATE_MT);
	lua_pushcfunction(L, deflate_userdata_gc);
	lua_setfield(L, -2, "__gc");
	lua_pop(L, 1);
	luaL_newmetatable(L, INFLATE_MT);
	lua_pushcfunction(L, inflate_userdata_gc);
	lua_setfield(L, -2, "__gc");
	lua_pop(L, 1);
	luaL_register(L, "larc.zlib", larc_zlib_Reg);
	lua_pushstring(L, zlibVersion());
	lua_setfield(L, -2, "ZLIB_VERSION");
	SETCONSTANT(Z_OK);
	SETCONSTANT(Z_STREAM_END);
	SETCONSTANT(Z_NEED_DICT);
	SETCONSTANT(Z_ERRNO);
	SETCONSTANT(Z_STREAM_ERROR);
	SETCONSTANT(Z_DATA_ERROR);
	SETCONSTANT(Z_MEM_ERROR);
	SETCONSTANT(Z_BUF_ERROR);
	SETCONSTANT(Z_VERSION_ERROR);
	return 1;
}
