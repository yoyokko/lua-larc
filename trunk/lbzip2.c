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

#include "bzlib.h"
#include "shared.h"

#define USE_SMALL_DECOMPRESS	0

#define BZ2COMPRESS_MT  	"larc.bzip2.deflate"
#define BZ2DECOMPRESS_MT	"larc.bzip2.inflate"

/* Living dangerously. */
typedef struct
{
	void*		handle;
	char		buf[BZ_MAX_UNUSED];
	int			bufn;
	char		write;
	bz_stream	stream;
	int			lastErr;
	char		init;
} bz2_file;

static const char * bz2_error(int errnum)
{
	bz2_file B;
	int n;
	B.lastErr = errnum;
	return BZ2_bzerror((BZFILE*)&B, &n);
}

typedef struct bzip2_userdata
{
	bz_stream z;
	int status;
	int result;
	int flush;
} bz_userdata;

static int compress_userdata_gc(lua_State *L)
{
	bz_userdata *ud = (bz_userdata*)lua_touserdata(L, 1);
	BZ2_bzCompressEnd(&ud->z);
	return 0;
}

static int decompress_userdata_gc(lua_State *L)
{
	bz_userdata *ud = (bz_userdata*)lua_touserdata(L, 1);
	BZ2_bzDecompressEnd(&ud->z);
	return 0;
}

static int compress_to_buffer(lua_State *L, bz_userdata *ud)
{
	luaL_Buffer B;
	luaL_buffinit(L, &B);
	do
	{
		ud->z.next_out = luaL_prepbuffer(&B);
		ud->z.avail_out = LUAL_BUFFERSIZE;
		if ((ud->status = BZ2_bzCompress(&ud->z, ud->flush)) < BZ_OK)
			break;
		luaL_addsize(&B, LUAL_BUFFERSIZE - ud->z.avail_out);
	}
	while (ud->z.avail_out == 0);
	switch (ud->status)
	{
		case BZ_RUN_OK: case BZ_FLUSH_OK: case BZ_FINISH_OK:
			ud->status = BZ_OK;
		case BZ_OK: case BZ_STREAM_END:
			if (ud->z.avail_in != 0)
				return luaL_error(L, "unknown failure in bzCompress");
	}
	luaL_pushresult(&B);
	return 1;
}

static int protected_compress_to_buffer(lua_State *L)
{
	bz_userdata *ud = (bz_userdata*)lua_touserdata(L, 1);
	compress_to_buffer(L, ud);
	ud->result = luaL_ref(L, LUA_REGISTRYINDEX);
	return 0;
}

static int compress_call(lua_State *L)
{
	bz_userdata *ud = (bz_userdata*)lua_touserdata(L, lua_upvalueindex(1));
	size_t len;
	const char *str = luaL_optlstring(L, 1, NULL, &len);
	if (str != NULL)
	{
		ud->z.next_in = (char*)str;
		ud->z.avail_in = len;
		ud->flush = BZ_RUN;
	}
	else
	{
		ud->z.next_in = (char*)"";
		ud->z.avail_in = 0;
		ud->flush = BZ_FINISH;
	}
	compress_to_buffer(L, ud);
	lua_pushinteger(L, len - ud->z.avail_in);
	lua_pushinteger(L, ud->status);
	return 3;
}

/**
 * Compress a string.
 * options:
 *   blocksize=[1,9]
 *   workfactor=[0,250]
 */
static int larc_bzip2_compress(lua_State *L)
{
	int blocksize = 6,
		workfactor = 0;
	size_t len;
	const char *str = luaL_checklstring(L, 1, &len);
	bz_userdata ud;

	if (lua_gettop(L) > 1)
	{
		luaL_checktype(L, 2, LUA_TTABLE);
		GETINT2OPTION(2,blocksize,level);
		GETINTOPTION(2,workfactor);
	}
	
	ud.z.bzalloc = NULL;
	ud.z.bzfree = NULL;
	ud.z.opaque = NULL;
	
	ud.status = BZ2_bzCompressInit(&ud.z, blocksize, 0, workfactor);
	if (ud.status != BZ_OK)
	{
		lua_pushnil(L);
		lua_pushstring(L, bz2_error(ud.status));
		lua_pushinteger(L, ud.status);
		return 3;
	}
	
	ud.z.next_in = (char*)str;
	ud.z.avail_in = len;
	ud.result = -1;
	ud.flush = BZ_FINISH;
	if (0 != lua_cpcall(L, protected_compress_to_buffer, &ud))
	{
		BZ2_bzCompressEnd(&ud.z);
		return lua_error(L);
	}
	BZ2_bzCompressEnd(&ud.z);
	
	if (ud.result != -1)
	{
		lua_rawgeti(L, LUA_REGISTRYINDEX, ud.result);
		luaL_unref(L, LUA_REGISTRYINDEX, ud.result);
		lua_pushinteger(L, len - ud.z.avail_in);
	}
	else
	{
		lua_pushnil(L);
		lua_pushstring(L, bz2_error(ud.status));
	}
	lua_pushinteger(L, ud.status);
	return 3;
}

/**
 * Create a compress function.
 * options:
 *   blocksize=[1,9]
 *   workfactor=[0,250]
 */
static int larc_bzip2_compressor(lua_State *L)
{
	int blocksize = 6,
		workfactor = 0;
	bz_userdata *ud;

	if (lua_gettop(L) > 0)
	{
		luaL_checktype(L, 1, LUA_TTABLE);
		GETINT2OPTION(1,blocksize,level);
		GETINTOPTION(1,workfactor);
	}
	
	ud = (bz_userdata*)lua_newuserdata(L, sizeof(bz_userdata));
	luaL_getmetatable(L, BZ2COMPRESS_MT);
	lua_setmetatable(L, -2);
	
	ud->z.bzalloc = NULL;
	ud->z.bzfree = NULL;
	ud->z.opaque = NULL;
	
	ud->status = BZ2_bzCompressInit(&ud->z, blocksize, 0, workfactor);
	if (ud->status != BZ_OK)
	{
		lua_pushnil(L);
		lua_pushstring(L, bz2_error(ud->status));
		lua_pushinteger(L, ud->status);
		return 3;
	}
	
	lua_pushcclosure(L, compress_call, 1);
	return 1;
}

static int decompress_to_buffer(lua_State *L, bz_userdata *ud)
{
	luaL_Buffer B;
	luaL_buffinit(L, &B);
	do
	{
		ud->z.next_out = luaL_prepbuffer(&B);
		ud->z.avail_out = LUAL_BUFFERSIZE;
		ud->status = BZ2_bzDecompress(&ud->z);
		if (ud->status != BZ_OK && ud->status != BZ_STREAM_END)
			break;
		luaL_addsize(&B, LUAL_BUFFERSIZE - ud->z.avail_out);
	}
	while (ud->z.avail_out == 0);
	if (ud->status == BZ_OK && ud->z.avail_in != 0)
		return luaL_error(L, "unknown failure in bzDecompress");
	/*
	if (ud->status == BZ_STREAM_END && ud->z.avail_in != 0)
		return luaL_error(L, "unhandled trailing data in bzip2 stream");
	*/
	luaL_pushresult(&B);
	return 1;
}

static int protected_decompress_to_buffer(lua_State *L)
{
	bz_userdata *ud = (bz_userdata*)lua_touserdata(L, 1);
	decompress_to_buffer(L, ud);
	ud->result = luaL_ref(L, LUA_REGISTRYINDEX);
	return 0;
}

static int decompress_call(lua_State *L)
{
	bz_userdata *ud = (bz_userdata*)lua_touserdata(L, lua_upvalueindex(1));
	size_t len;
	const char *str = luaL_optlstring(L, 1, NULL, &len);
	if (len > 0)
	{
		ud->z.next_in = (char*)str;
		ud->z.avail_in = len;
		decompress_to_buffer(L, ud);
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
 * options:
 */
static int larc_bzip2_decompress(lua_State *L)
{
	size_t len;
	const char *str = luaL_checklstring(L, 1, &len);
	bz_userdata ud;

	ud.z.bzalloc = NULL;
	ud.z.bzfree = NULL;
	ud.z.opaque = NULL;
	ud.z.next_in = (char*)str;
	ud.z.avail_in = len;
	
	ud.status = BZ2_bzDecompressInit(&ud.z, 0, USE_SMALL_DECOMPRESS);
	if (ud.status != BZ_OK)
	{
		lua_pushnil(L);
		lua_pushstring(L, bz2_error(ud.status));
		lua_pushinteger(L, ud.status);
		return 3;
	}
	
	ud.result = -1;
	if (0 != lua_cpcall(L, protected_decompress_to_buffer, &ud))
	{
		BZ2_bzDecompressEnd(&ud.z);
		return lua_error(L);
	}
	BZ2_bzDecompressEnd(&ud.z);
	
	if (ud.result != -1)
	{
		lua_rawgeti(L, LUA_REGISTRYINDEX, ud.result);
		luaL_unref(L, LUA_REGISTRYINDEX, ud.result);
		lua_pushinteger(L, len - ud.z.avail_in);
	}
	else
	{
		lua_pushnil(L);
		lua_pushstring(L, bz2_error(ud.status));
	}
	lua_pushinteger(L, ud.status);
	return 3;
}

/**
 * Create an decompress function.
 * options:
 */
static int larc_bzip2_decompressor(lua_State *L)
{
	bz_userdata *ud;

	ud = (bz_userdata*)lua_newuserdata(L, sizeof(bz_userdata));
	luaL_getmetatable(L, BZ2DECOMPRESS_MT);
	lua_setmetatable(L, -2);
	
	ud->z.bzalloc = NULL;
	ud->z.bzfree = NULL;
	ud->z.opaque = NULL;
	ud->z.next_in = NULL;
	ud->z.avail_in = 0;
	
	ud->status = BZ2_bzDecompressInit(&ud->z, 0, USE_SMALL_DECOMPRESS);
	if (ud->status != BZ_OK)
	{
		lua_pushnil(L);
		lua_pushstring(L, bz2_error(ud->status));
		lua_pushinteger(L, ud->status);
		return 3;
	}

	lua_pushcclosure(L, decompress_call, 1);
	return 1;
}

#ifdef _WIN32
#undef LUAMOD_API
#define LUAMOD_API      __declspec(dllexport)
#endif

static const luaL_Reg larc_bzip2_Reg[] = 
{
	{"compress", larc_bzip2_compress},
	{"decompress", larc_bzip2_decompress},
	{"compressor", larc_bzip2_compressor},
	{"decompressor", larc_bzip2_decompressor},
	{NULL, NULL}
};

LUAMOD_API int luaopen_larc_bzip2(lua_State *L)
{
	luaL_newmetatable(L, BZ2COMPRESS_MT);
	lua_pushcfunction(L, compress_userdata_gc);
	lua_setfield(L, -2, "__gc");
	lua_pop(L, 1);
	luaL_newmetatable(L, BZ2DECOMPRESS_MT);
	lua_pushcfunction(L, decompress_userdata_gc);
	lua_setfield(L, -2, "__gc");
	lua_pop(L, 1);
	luaL_register(L, "larc.bzip2", larc_bzip2_Reg);
	lua_pushstring(L, BZ2_bzlibVersion());
	lua_setfield(L, -2, "BZLIB_VERSION");
	SETCONSTANT(BZ_OK);
	SETCONSTANT(BZ_STREAM_END);
	SETCONSTANT(BZ_CONFIG_ERROR);
	SETCONSTANT(BZ_SEQUENCE_ERROR);
	SETCONSTANT(BZ_PARAM_ERROR);
	SETCONSTANT(BZ_DATA_ERROR);
	SETCONSTANT(BZ_MEM_ERROR);
	SETCONSTANT(BZ_IO_ERROR);
	SETCONSTANT(BZ_DATA_ERROR_MAGIC);
	SETCONSTANT(BZ_UNEXPECTED_EOF);
	SETCONSTANT(BZ_OUTBUFF_FULL);
	return 1;
}
