#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "lua.h"
#include "lauxlib.h"

/* #define NO_LONG_DOUBLE */

/*
** {======================================================
** Library for packing/unpacking structures.
** $Id: struct.c,v 1.2 2008/04/18 20:06:01 roberto Exp $
** =======================================================
*/
/*
** Valid formats:
** > - big endian
** < - little endian
** ![num] - alignment
** x - padding
** b/B - signed/unsigned byte (8 bits)
** h/H - signed/unsigned short (16 bits)
** l/L - signed/unsigned long (32 bits)
** q/Q - signed/unsigned quad (64 bits)
** i/In - signed/unsigned integer with size `n' (default is size of int)
** cn - sequence of `n' chars (from/to a string); when packing, n==0 means
        the whole string; when unpacking, n==0 means use the previous
        read number as the string length
** s - zero-terminated string
** f - float
** d - double
** ' ' - ignored
*/

/* is 'x' a power of 2? */
#define isp2(x)        ((x) > 0 && ((x) & ((x) - 1)) == 0)

/* type that can hold the largest integer */
typedef long long    longestint;
typedef unsigned long long    ulongestint;

#define LONGESTMAX	((1LL<<48)-1)
#define LONGESTMIN	(0-(1LL<<48))

/* dummy structure to get alignment requirements */
struct cD {
  char c;
  double d;
};


/* endian options */
#define BIG    0
#define LITTLE    1


static union {
  int dummy;
  char endian;
} const native = {1};


typedef struct Header {
  int endian;
  int align;
} Header;

typedef longestint largeinteger_t;
#define LARGETYPE	"large integer"

#define PADDING        (sizeof(struct cD) - sizeof(double))
#define MAXALIGN      (PADDING > (sizeof(largeinteger_t) ? PADDING : sizeof(largeinteger_t)))


static void newlargeint (lua_State *L, largeinteger_t i) {
  largeinteger_t *li = (largeinteger_t*)lua_newuserdata(L, sizeof(largeinteger_t));
  *li = i;
  luaL_getmetatable(L, LARGETYPE);
  lua_setmetatable(L, -2);
}

static largeinteger_t getlargeint (lua_State *L, int i) {
  largeinteger_t li;
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
      li = *((largeinteger_t*)luaL_checkudata(L, i, LARGETYPE));
      return li;
    }
  }
  return 0;
}


static size_t getnum (const char **fmt, size_t df) {
  if (!isdigit(**fmt))  /* no number? */
    return df;  /* return default value */
  else {
    size_t a = 0;
    do {
      a = a*10 + *((*fmt)++) - '0';
    } while (isdigit(**fmt));
    return a;
  }
}


#define defaultoptions(h)    ((h)->endian = native.endian, (h)->align = 1)



static size_t optsize (lua_State *L, char opt, const char **fmt, size_t *rep) {
  switch (opt) {
    case 'B': case 'b': *rep = getnum(fmt, 1); return 1;
    case 'H': case 'h': *rep = getnum(fmt, 1); return 2;
    case 'L': case 'l': *rep = getnum(fmt, 1); return 4;
    case 'Q': case 'q': *rep = getnum(fmt, 1); return 8;
    case 'f': *rep = getnum(fmt, 1);  return sizeof(float);
    case 'd': *rep = getnum(fmt, 1);  return sizeof(double);
    case 'x': case 'c': return getnum(fmt, 1);
    case 's': return getnum(fmt, 0);
    case ' ': case '<': case '>': case '!': return 0;
    case 'i': case 'I': {
      size_t sz = getnum(fmt, sizeof(int));
      if (!isp2(sz))
        luaL_error(L, "integral size %d is not a power of 2", sz);
      if (sz > sizeof(largeinteger_t))
        luaL_error(L, "integral size %d is too large", sz);
      return sz;
    }
    default: {
      const char *msg = lua_pushfstring(L, "invalid format option [%c]", opt);
      return luaL_argerror(L, 1, msg);
    }
  }
}


static int gettoalign (size_t len, Header *h, int opt, size_t size) {
  if (size == 0 || opt == 'c') return 0;
  if (size > (size_t)h->align) size = h->align;  /* respect max. alignment */
  return  (size - (len & (size - 1))) & (size - 1);
}


static void commoncases (lua_State *L, int opt, const char **fmt, Header *h) {
  switch (opt) {
    case  ' ': return;  /* ignore white spaces */
    case '>': h->endian = BIG; return;
    case '<': h->endian = LITTLE; return;
    case '!': {
      int a = getnum(fmt, MAXALIGN);
      if (!isp2(a))
        luaL_error(L, "alignment %d is not a power of 2", a);
      h->align = a;
      return;
    }
    default: assert(0);
  }
}


static void putinteger (lua_State *L, luaL_Buffer *b, int arg, int endian,
                        int size) {
  ulongestint value = getlargeint(L, arg);
  if (endian == LITTLE) {
    int i;
    for (i = 0; i < size; i++)
      luaL_addchar(b, (value >> 8*i) & 0xff);
  }
  else {
    int i;
    for (i = size - 1; i >= 0; i--)
      luaL_addchar(b, (value >> 8*i) & 0xff);
  }
}


static void correctbytes (char *b, int size, int endian) {
  if (endian != native.endian) {
    int i = 0;
    while (i < --size) {
      char temp = b[i];
      b[i++] = b[size];
      b[size] = temp;
    }
  }
}


static int b_pack (lua_State *L) {
  luaL_Buffer b;
  const char *fmt = luaL_checkstring(L, 1);
  Header h;
  int arg = 2;
  size_t totalsize = 0;
  defaultoptions(&h);
  lua_pushnil(L);  /* mark to separate arguments from string buffer */
  luaL_buffinit(L, &b);
  while (*fmt != '\0') {
    int opt = *fmt++;
    size_t rep = 1;
    size_t size = optsize(L, opt, &fmt, &rep);
    while (rep--) {
      int toalign = gettoalign(totalsize, &h, opt, size);
      totalsize += toalign;
      while (toalign-- > 0) luaL_addchar(&b, '\0');
      switch (opt) {
        case 'b': case 'B': case 'h': case 'H':
        case 'l': case 'L': case 'i': case 'I':
        case 'q': case 'Q': {  /* integer types */
          putinteger(L, &b, arg++, h.endian, size);
          break;
        }
        case 'x': {
          size_t l;
          for (l=0; l<size; l++)
            luaL_addchar(&b, '\0');
          break;
        }
        case 'f': {
          float f = (float)luaL_checknumber(L, arg++);
          correctbytes((char *)&f, size, h.endian);
          luaL_addlstring(&b, (char *)&f, size);
          break;
        }
        case 'd': {
          double d = luaL_checknumber(L, arg++);
          correctbytes((char *)&d, size, h.endian);
          luaL_addlstring(&b, (char *)&d, size);
          break;
        }
        case 'c': case 's': {
          size_t l;
          size_t sz = size;
          const char *s = luaL_checklstring(L, arg++, &l);
          if (size == 0) size = l;
          if (l < size) {
            luaL_addlstring(&b, s, l);
            while (l++ > size)
              luaL_addchar(&b, '\0');
          }
          else
            luaL_addlstring(&b, s, size);
          if (opt == 's' && sz == 0) {
            luaL_addchar(&b, '\0');  /* add zero at the end */
            size++;
          }
          break;
        }
        default: commoncases(L, opt, &fmt, &h);
      }
      totalsize += size;
    }
  }
  luaL_pushresult(&b);
  return 1;
}


static void getinteger (lua_State *L, const char *buff, int endian,
                        int issigned, int size) {
  ulongestint li = 0;
  int i;
  if (endian == BIG) {
    for (i = 0; i < size; i++)
      li |= ((ulongestint)(unsigned char)buff[size - i - 1]) << (i*8);
  }
  else {
    for (i = 0; i < size; i++)
      li |= ((ulongestint)(unsigned char)buff[i]) << (i*8);
  }
  /* signed format */
  if (issigned) {
    ulongestint mask = ~(0ULL) << (size*8 - 1);
    if (li & mask)  /* negative value? */
      li |= mask;  /* signal extension */
    if ((longestint)li > LONGESTMAX || (longestint)li < LONGESTMIN)
      newlargeint(L, li);
    else
      lua_pushnumber(L, (lua_Number)(longestint)li);
    return;
  }
  /* unsigned format */
  if (li > LONGESTMAX)
    newlargeint(L, li);
  else
    lua_pushnumber(L, (lua_Number)li);
  return;
}


static int b_unpack (lua_State *L) {
  Header h;
  const char *fmt = luaL_checkstring(L, 1);
  size_t ld;
  const char *data = luaL_checklstring(L, 2, &ld);
  size_t pos = luaL_optinteger(L, 3, 1) - 1;
  defaultoptions(&h);
  lua_settop(L, 2);
  if (pos >= ld) {
    lua_pushnil(L);
    lua_pushliteral(L, "data string too short");
    return 2;
  }
  while (*fmt) {
    int opt = *fmt++;
    size_t rep = 1;
    size_t size = optsize(L, opt, &fmt, &rep);
    while (rep--) {
      pos += gettoalign(pos, &h, opt, size);
      if (pos+size > ld) {
        lua_pushnil(L);
        lua_pushliteral(L, "data string too short");
        return 2;
      }
      switch (opt) {
        case 'b': case 'B': case 'h': case 'H':
        case 'l': case 'L': case 'i':  case 'I':
        case 'q': case 'Q': {  /* integer types */
          int issigned = islower(opt);
          getinteger(L, data+pos, h.endian, issigned, size);
          break;
        }
        case 'x': {
          break;
        }
        case 'f': {
          float f;
          memcpy(&f, data+pos, size);
          correctbytes((char *)&f, sizeof(f), h.endian);
          lua_pushnumber(L, f);
          break;
        }
        case 'd': {
          double d;
          memcpy(&d, data+pos, size);
          correctbytes((char *)&d, sizeof(d), h.endian);
          lua_pushnumber(L, d);
          break;
        }
        case 'c': {
          if (size == 0) {
            if (!lua_isnumber(L, -1))
              luaL_error(L, "format `c0' needs a previous size");
            size = lua_tonumber(L, -1);
            lua_pop(L, 1);
            if (pos+size > ld) {
              lua_pushnil(L);
              lua_pushliteral(L, "data string too short");
              return 2;
            }
          }
          lua_pushlstring(L, data+pos, size);
          break;
        }
        case 's': {
          size_t sz = size==0 ? (ld-pos) : size;
          const char *e = (const char *)memchr(data+pos, '\0', sz);
          if (e == NULL) {
            if (size == 0) {
              lua_pushnil(L);
              lua_pushliteral(L, "unfinished string in data");
              return 2;
            }
          }
          else {
            sz = e - (data+pos);
            if (size == 0)
              size = sz + 1;
          }
          lua_pushlstring(L, data+pos, sz);
          break;
        }
        default: commoncases(L, opt, &fmt, &h);
      }
      pos += size;
    }
  }
  lua_pushinteger(L, pos + 1);
  return lua_gettop(L) - 2;
}

static int largeinttostring (lua_State *L) {
  char s[64];
  char *minus = "";
  largeinteger_t li = getlargeint(L, 1);
  if (li < 0) {
    minus = "-";
    li = -li;
  }
  sprintf(s, "%s0x%"PRIX64, minus, li);
  lua_pushstring(L, s);
  return 1;
}

static int largeinttonumber (lua_State *L) {
  largeinteger_t li = getlargeint(L, 1);
  lua_pushnumber(L, (lua_Number)li);
  return 1;
}

static int neglargeint (lua_State *L) {
  largeinteger_t li = getlargeint(L, 1);
  newlargeint(L, -li);
  return 1;
}

static int addlargeint (lua_State *L) {
  largeinteger_t a, b;
  a = getlargeint(L, 1);
  b = getlargeint(L, 2);
  a += b;
  newlargeint(L, a);
  return 1;
}

static int sublargeint (lua_State *L) {
  largeinteger_t a, b;
  a = getlargeint(L, 1);
  b = getlargeint(L, 2);
  a -= b;
  newlargeint(L, a);
  return 1;
}

static int mullargeint (lua_State *L) {
  largeinteger_t a, b;
  a = getlargeint(L, 1);
  b = getlargeint(L, 2);
  a *= b;
  newlargeint(L, a);
  return 1;
}

static int divlargeint (lua_State *L) {
  largeinteger_t a, b;
  a = getlargeint(L, 1);
  b = getlargeint(L, 2);
  a /= b;
  newlargeint(L, a);
  return 1;
}

static int modlargeint (lua_State *L) {
  largeinteger_t a, b;
  a = getlargeint(L, 1);
  b = getlargeint(L, 2);
  a /= b;
  newlargeint(L, a);
  return 1;
}

static int powlargeint (lua_State *L) {
  largeinteger_t a, b, c;
  a = getlargeint(L, 1);
  b = getlargeint(L, 2);
#ifndef NO_LONG_DOUBLE
  c = powl(a,b);
#else
  c = pow(a,b);
#endif
  newlargeint(L, c);
  return 1;
}

static int eqlargeint (lua_State *L) {
  largeinteger_t a, b;
  a = getlargeint(L, 1);
  b = getlargeint(L, 2);
  lua_pushboolean(L, a==b);
  return 1;
}

static int ltlargeint (lua_State *L) {
  largeinteger_t a, b;
  a = getlargeint(L, 1);
  b = getlargeint(L, 2);
  lua_pushboolean(L, a<b);
  return 1;
}

static int lelargeint (lua_State *L) {
  largeinteger_t a, b;
  a = getlargeint(L, 1);
  b = getlargeint(L, 2);
  lua_pushboolean(L, a<=b);
  return 1;
}

static int b_largeint (lua_State *L) {
  largeinteger_t li = getlargeint(L, 1);
  newlargeint(L, li);
  return 1;
}

/* }====================================================== */


#ifdef _WIN32
#undef LUAMOD_API
#define LUAMOD_API	__declspec(dllexport)
#endif


static const luaL_reg largeintMT[] = {
  {"__add", addlargeint},
  {"__sub", sublargeint},
  {"__mul", mullargeint},
  {"__div", divlargeint},
  {"__mod", modlargeint},
  {"__pow", powlargeint},
  {"__unm", neglargeint},
  {"__eq", eqlargeint},
  {"__lt", ltlargeint},
  {"__le", lelargeint},
  {"__tostring", largeinttostring},
  {"tonumber", largeinttonumber},
  {NULL, NULL}
};

static const struct luaL_reg thislib[] = {
  {"pack", b_pack},
  {"unpack", b_unpack},
  {"largeinteger", b_largeint},
  {NULL, NULL}
};


LUAMOD_API int luaopen_larc_struct (lua_State *L) {
  luaL_newmetatable(L, LARGETYPE);
  luaL_register(L, NULL, largeintMT);
  lua_pushliteral(L, "__index");
  lua_pushvalue(L, -2);
  lua_settable(L, -3);
  lua_pop(L, 1);
  luaL_register(L, "larc.struct", thislib);
  return 1;
}


