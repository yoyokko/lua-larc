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

/* Read an option from the argument table */
#define GETINTOPTION(arg,opt)	{ \
		lua_getfield(L, arg, #opt); \
		opt = luaL_optint(L, -1, opt); \
		lua_pop(L, 1); }
/* Read an option with alternate name from the argument table */
#define GETINT2OPTION(arg,opt,alt)	{ \
		lua_getfield(L, arg, #opt); \
		if (lua_isnil(L, -1)) { \
			lua_pop(L, 1); \
			lua_getfield(L, arg, #alt); \
		} \
		opt = luaL_optint(L, -1, opt); \
		lua_pop(L, 1); }
/* Set the value of a constant in the table at the top of the stack. */
#define SETCONSTANT(c)	{ \
		lua_pushinteger(L, c); \
		lua_setfield(L, -2, #c); }
/* Set a negative constant in the table at the top of the stack. */
#define SETERRORCONSTANT(c)	{ \
		lua_pushinteger(L, -c); \
		lua_setfield(L, -2, #c); }

/* Helper for 5.2 compatibility. Will need to be rewritten many times. */
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
