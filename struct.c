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
** u - UTF-16 zero-terminated string (from UTF-8 string)
** U - 32-bit unicode string, zero-terminated
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

static void * memchr2 (const void * ptr, int c, size_t nb) {
  unsigned short * p = (unsigned short *)ptr;
  unsigned short * q = p + (nb / 2);
  while (p < q) {
    if (*p++ == c)
      return p-1;
  }
  return NULL;
}

static void * memchr4 (const void * ptr, int c, size_t nb) {
  unsigned long * p = (unsigned long *)ptr;
  unsigned long * q = p + (nb / 4);
  while (p < q) {
    if (*p++ == c)
      return p-1;
  }
  return NULL;
}


static void strtounicode (lua_State *L, int arg, int width, int endian) {
  luaL_Buffer b;
  unsigned long uni;
  size_t l;
  const unsigned char *s = (const unsigned char *)luaL_checklstring(L, arg, &l);
  const unsigned char *t = s + l;
  luaL_buffinit(L, &b);
  while (s < t) {
    if (*s < 0x80)
      uni = *s++;
    else if (*s < 0xC0)
      luaL_argerror(L, arg, "invalid utf-8");
    else if (*s < 0xE0) {
      if (t-s < 2) luaL_argerror(L, arg, "invalid utf-8");
      if ((s[1]&0xC0) != 0x80) luaL_argerror(L, arg, "invalid utf-8");
      uni = ((unsigned long)(s[0]&0x1F) << 6) | (s[1]&0x3F);
      if (uni < 0x80) luaL_argerror(L, arg, "invalid utf-8");
      s += 2;
    }
    else if (*s < 0xF0) {
      if (t-s < 3) luaL_argerror(L, arg, "invalid utf-8");
      if (((s[1]&0xC0) != 0x80) || ((s[2]&0xC0) != 0x80))
       luaL_argerror(L, arg, "invalid utf-8");
      uni = ((unsigned long)(s[0]&0xF) << 12) | 
            ((unsigned long)(s[1]&0x3F) << 6) | (s[2]&0x3F);
      if (uni < 0x800 || (uni&0xF800) == 0xD800) luaL_argerror(L, arg, "invalid utf-8");
      s += 3;
    }
    else if (*s < 0xF8) {
      if (t-s < 4) luaL_argerror(L, arg, "invalid utf-8");
      if (((s[1]&0xC0) != 0x80) || ((s[2]&0xC0) != 0x80) || ((s[3]&0xC0) != 0x80))
       luaL_argerror(L, arg, "invalid utf-8");
      uni = ((unsigned long)(s[0]&0x7) << 18) | 
            ((unsigned long)(s[1]&0x3F) << 12) | 
            ((unsigned long)(s[2]&0x3F) << 6) | (s[3]&0x3F);
      if (uni < 0x10000) luaL_argerror(L, arg, "invalid utf-8");
      s += 4;
    }
    else if (*s < 0xFC) {
      if (t-s < 5) luaL_argerror(L, arg, "invalid utf-8");
      if (((s[1]&0xC0) != 0x80) || ((s[2]&0xC0) != 0x80) ||
          ((s[3]&0xC0) != 0x80) || ((s[4]&0xC0) != 0x80))
       luaL_argerror(L, arg, "invalid utf-8");
      uni = ((unsigned long)(s[0]&0x3) << 24) | 
            ((unsigned long)(s[1]&0x3F) << 18) | 
            ((unsigned long)(s[2]&0x3F) << 12) | 
            ((unsigned long)(s[3]&0x3F) << 6) | (s[4]&0x3F);
      if (uni < 0x200000) luaL_argerror(L, arg, "invalid utf-8");
      s += 5;
    }
    else if (*s < 0xFE) {
      if (t-s < 6) luaL_argerror(L, arg, "invalid utf-8");
      if (((s[1]&0xC0) != 0x80) || ((s[2]&0xC0) != 0x80) ||
          ((s[3]&0xC0) != 0x80) || ((s[4]&0xC0) != 0x80) || ((s[5]&0xC0) != 0x80))
       luaL_argerror(L, arg, "invalid utf-8");
      uni = ((unsigned long)(s[0]&0x1) << 30) | 
            ((unsigned long)(s[1]&0x3F) << 24) | 
            ((unsigned long)(s[2]&0x3F) << 18) | 
            ((unsigned long)(s[3]&0x3F) << 12) | 
            ((unsigned long)(s[4]&0x3F) << 6) | (s[5]&0x3F);
      if (uni < 0x4000000) luaL_argerror(L, arg, "invalid utf-8");
      s += 6;
    }
    else
      luaL_argerror(L, arg, "invalid utf-8");
    if (width == 2) {
      if (uni >= 0x110000)
        luaL_argerror(L, arg, "unicode character out of range");
      if (uni < 0x10000) {
        if (endian == BIG) {
          luaL_addchar(&b, uni>>8);
          luaL_addchar(&b, uni);
        }
        else {
          luaL_addchar(&b, uni);
          luaL_addchar(&b, uni>>8);
        }
      }
      else {
        unsigned long w1,w2;
        uni -= 0x10000;
        w1 = (uni>>10) | 0xD800;
        w2 = (uni&0x3FF) | 0xDC00;
        if (endian == BIG) {
          luaL_addchar(&b, w1>>8);
          luaL_addchar(&b, w1);
          luaL_addchar(&b, w2>>8);
          luaL_addchar(&b, w2);
        }
        else {
          luaL_addchar(&b, w1);
          luaL_addchar(&b, w1>>8);
          luaL_addchar(&b, w2);
          luaL_addchar(&b, w2>>8);
        }
      }
    }
    else {
      if (endian == BIG) {
        luaL_addchar(&b, uni>>24);
        luaL_addchar(&b, uni>>16);
        luaL_addchar(&b, uni>>8);
        luaL_addchar(&b, uni);
      }
      else {
        luaL_addchar(&b, uni);
        luaL_addchar(&b, uni>>8);
        luaL_addchar(&b, uni>>16);
        luaL_addchar(&b, uni>>24);
      }
    }
  }
  luaL_pushresult(&b);
  lua_replace(L, arg);
}

static void unicodetostr (lua_State *L, const char *str, size_t len, int width, int endian) {
  luaL_Buffer b;
  unsigned long uni;
  const unsigned char *s = (const unsigned char *)str;
  const unsigned char *t = s + (len / width) * width;
  luaL_buffinit(L, &b);
  while (s < t) {
    if (width == 4) {
      if (endian == BIG)
        uni = (s[0]<<24) | (s[1]<<16) | (s[2]<<8) | (s[3]);
      else
        uni = (s[0]) | (s[1]<<8) | (s[2]<<16) | (s[3]<<24);
      if ((uni&0xFFFFF800UL) == 0xD800) luaL_error(L, "invalid unicode character");
      s += 4;
    }
    else {
      if (endian == BIG)
        uni = (s[0]<<8) | (s[1]);
      else
        uni = (s[0]) | (s[1]<<8);
      s += 2;
      if ((uni & 0xF800) == 0xD800) {
        unsigned long w;
        if (s >= t)
          luaL_error(L, "invalid utf-16");
        if (endian == BIG)
          w = (s[0]<<8) | (s[1]);
        else
          w = (s[0]) | (s[1]<<8);
        if ((w & 0xFC00) != 0xDC00)
          luaL_error(L, "invalid utf-16");
        s += 2;
        uni = (((uni&0x3FF)<<10) | (w&0x3FF)) + 0x10000;
      }
    }
    if (uni < 0x80)
      luaL_addchar(&b, uni);
    else if (uni < 0x800) {
      luaL_addchar(&b, (uni>>6)|0xC0);
      luaL_addchar(&b, (uni&0x3F)|0x80);
    }
    else if (uni < 0x10000) {
      luaL_addchar(&b, (uni>>12)|0xE0);
      luaL_addchar(&b, ((uni>>6)&0x3F)|0x80);
      luaL_addchar(&b, (uni&0x3F)|0x80);
    }
    else if (uni < 0x200000) {
      luaL_addchar(&b, (uni>>18)|0xF0);
      luaL_addchar(&b, ((uni>>12)&0x3F)|0x80);
      luaL_addchar(&b, ((uni>>6)&0x3F)|0x80);
      luaL_addchar(&b, (uni&0x3F)|0x80);
    }
    else if (uni < 0x4000000) {
      luaL_addchar(&b, (uni>>24)|0xF8);
      luaL_addchar(&b, ((uni>>18)&0x3F)|0x80);
      luaL_addchar(&b, ((uni>>12)&0x3F)|0x80);
      luaL_addchar(&b, ((uni>>6)&0x3F)|0x80);
      luaL_addchar(&b, (uni&0x3F)|0x80);
    }
    else if (uni < 0x80000000UL) {
      luaL_addchar(&b, (uni>>30)|0xFC);
      luaL_addchar(&b, ((uni>>24)&0x3F)|0x80);
      luaL_addchar(&b, ((uni>>18)&0x3F)|0x80);
      luaL_addchar(&b, ((uni>>12)&0x3F)|0x80);
      luaL_addchar(&b, ((uni>>6)&0x3F)|0x80);
      luaL_addchar(&b, (uni&0x3F)|0x80);
    }
    else
      luaL_error(L, "unicode character out of range");
  }
  luaL_pushresult(&b);
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
    case 'u': return 2*getnum(fmt, 0);
    case 'U': return 4*getnum(fmt, 0);
    case ' ': case '<': case '>': case '!': return 0;
    case 'i': case 'I': {
      size_t sz = getnum(fmt, sizeof(int));
      /*
      if (!isp2(sz))
        luaL_error(L, "integral size %d is not a power of 2", sz);
      */
      if (!sz)
        luaL_error(L, "integral size must be greater than zero");
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
  if (opt == 'x' && size == 0) size = h->align;
  else if (size == 0 || opt == 'c' || opt == 's' || opt == 'x') return 0;
  else if (opt == 'u') size = 2;
  else if (opt == 'U') size = 4;
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
      if (!a)
        luaL_error(L, "alignment must be greater than zero");
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
        case 'u': case 'U':
          strtounicode(L, arg, opt=='U'?4:2, h.endian);
          /* continue as string */
        case 'c': case 's': {
          size_t l;
          size_t sz = size;
          const char *s = luaL_checklstring(L, arg++, &l);
          if (size == 0) size = l;
          if (l < size) {
            luaL_addlstring(&b, s, l);
            while (l++ < size)
              luaL_addchar(&b, '\0');
          }
          else
            luaL_addlstring(&b, s, size);
          if (opt == 's' && sz == 0) {
            luaL_addchar(&b, '\0');  /* add zero at the end */
            size++;
          }
          else if (opt == 'u' && sz == 0) {
            luaL_addchar(&b, '\0');
            luaL_addchar(&b, '\0');
            size+=2;
          }
          else if (opt == 'U' && sz == 0) {
            luaL_addchar(&b, '\0');
            luaL_addchar(&b, '\0');
            luaL_addchar(&b, '\0');
            luaL_addchar(&b, '\0');
            size+=4;
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
  size_t ld, ls;
  const char *data = luaL_checklstring(L, 2, &ld);
  size_t pos = luaL_optinteger(L, 3, 1) - 1;
  defaultoptions(&h);
  lua_settop(L, 2);
  if (pos >= ld) {
    lua_pushnil(L);
    lua_pushliteral(L, "data string too short");
    return 2;
  }
  ls = 1;
  while (*fmt) {
    int opt = *fmt++;
    size_t rep = 1;
    size_t size = optsize(L, opt, &fmt, &rep);
    if (size != 0 && opt != 'x')
      ls += rep;
  }
  luaL_checkstack(L, ls, "too many values to unpack");
  fmt = lua_tostring(L, 1);
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
        case 's': case 'u': case 'U': {
          size_t sz = size==0 ? (ld-pos) : size;
          const char *e;
          if (opt == 'U')
            e = (const char *)memchr4(data+pos, 0, sz);
          else if (opt == 'u')
            e = (const char *)memchr2(data+pos, 0, sz);
          else
            e = (const char *)memchr(data+pos, 0, sz);
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
              size = sz + (opt=='U'?4:opt=='u'?2:1);
          }
          if (opt != 's')
            unicodetostr(L, data+pos, sz, opt=='U'?4:2, h.endian);
          else
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

#define MAKELONG(a,b,c,d)	\
	((((unsigned long)(a))<<24)|\
	(((unsigned long)(b))<<16)|\
	(((unsigned long)(c))<<8)|\
	((unsigned long)(d)))
static size_t _tobase85 (ulongestint li, char *buf) {
  unsigned char bytes[16];
  size_t nbuf;
  unsigned long l;
  int i, n = 0;
  if (li == 0) {
    buf[0] = '!';
	buf[1] = '!';
    return 2;
  }
  for (i=0; i<16; i++, li>>=8) {
    if (0!=(bytes[i] = (unsigned char)(li&255)))
      n = i;
  }
  nbuf = 0;
  for (; n>=3; n-=4) {
    l = MAKELONG(bytes[n],bytes[n-1],bytes[n-2],bytes[n-3]);
    if (l == 0)
      buf[nbuf++] = 'z';
    else {
      for (i=0; i<5; i++) {
        buf[4+nbuf-i] = '!' + (l%85);
        l /= 85;
      }
      nbuf += 5;
    }
  }
  switch (n) {
  case 0:
    l = MAKELONG(bytes[0],0,0,0);
    l /= 85*85*85;
    n = 2;
    break;
  case 1:
    l = MAKELONG(bytes[1],bytes[0],0,0);
    l /= 85*85;
    n = 3;
    break;
  case 2:
    l = MAKELONG(bytes[2],bytes[1],bytes[0],0);
    l /= 85;
    n = 4;
    break;
  default:
    return nbuf;
  }
  nbuf += n;
  for (i=1; i<=n; i++) {
    buf[nbuf-i] = '!' + (l%85);
	l /= 85;
  }
  return nbuf;
}

static size_t _tobase64 (ulongestint li, char *buf) {
  unsigned char bytes[16];
  size_t nbuf;
  unsigned long l;
  int i, n = 0;
  for (i=0; i<16; i++, li>>=8) {
    if (0!=(bytes[i] = (unsigned char)(li&255)))
      n = i;
  }
  nbuf = 0;
  for (; n>=2; n-=3) {
    l = MAKELONG(0,bytes[n],bytes[n-1],bytes[n-2]);
    for (i=0; i<4; i++) {
      unsigned char c = l % 64;
      if (c == 63) c = '/';
      else if (c == 62) c = '+';
      else if (c >= 52) c += '0' - 52;
      else if (c >= 26) c += 'a' - 26;
      else c += 'A';
      buf[3+nbuf-i] = c;
      l /= 64;
    }
    nbuf += 4;
  }
  switch (n) {
  case 0:
    l = MAKELONG(0,bytes[0],0,0);
    l /= 64*64;
    n = 2;
    break;
  case 1:
    l = MAKELONG(0,bytes[1],bytes[0],0);
    l /= 64;
    n = 3;
    break;
  default:
    return nbuf;
  }
  nbuf += n;
  for (i=1; i<=n; i++) {
    unsigned char c = l % 64;
    if (c == 63) c = '/';
    else if (c == 62) c = '+';
    else if (c >= 52) c += '0' - 52;
    else if (c >= 26) c += 'a' - 26;
    else c += 'A';
    buf[nbuf-i] = c;
    l /= 64;
  }
  for (i=n; i<4; i++)
    buf[nbuf++] = '=';
  return nbuf;
}

static size_t _tobase32 (ulongestint li, char *buf) {
  unsigned char bytes[16];
  size_t nbuf;
  unsigned long l,k;
  int i, n = 0;
  for (i=0; i<16; i++, li>>=8) {
    if (0!=(bytes[i] = (unsigned char)(li&255)))
      n = i;
  }
  nbuf = 0;
  for (; n>=4; n-=5) {
    l = MAKELONG(0,bytes[n],bytes[n-1],bytes[n-2]);
	k = MAKELONG(0,bytes[n-2]&15,bytes[n-3],bytes[n-4]);
	l /= 16;
    for (i=0; i<4; i++) {
      unsigned char c = l % 32;
      if (c >= 26) c += '2' - 26;
      else c += 'A';
      buf[3+nbuf-i] = c;
      l /= 32;
	  c = k % 32;
      if (c >= 26) c += '2' - 26;
      else c += 'A';
      buf[7+nbuf-i] = c;
      k /= 32;
    }
    nbuf += 8;
  }
  switch (n) {
  case 0:
    k = 0;
    l = MAKELONG(0,bytes[0],0,0);
    l /= 32*32*16;
    n = 2;
    break;
  case 1:
    k = 0;
    l = MAKELONG(0,bytes[1],bytes[0],0);
    l /= 16;
    n = 4;
    break;
  case 2:
    k = MAKELONG(0,bytes[0]&15,0,0);
    k /= 32*32*32;
    l = MAKELONG(0,bytes[2],bytes[1],bytes[0]);
    l /= 16;
    n = 5;
    break;
  case 3:
    k = MAKELONG(0,bytes[1]&15,bytes[0],0);
    k /= 32;
    l = MAKELONG(0,bytes[3],bytes[2],bytes[1]);
    l /= 16;
    n = 7;
    break;
  default:
    return nbuf;
  }
  nbuf += n;
  i = 1;
  for (; i<=n-4; i++) {
    unsigned char c = k % 32;
    if (c >= 26) c += '2' - 26;
    else c += 'A';
    buf[nbuf-i] = c;
    k /= 32;
  }
  for (; i<=n; i++) {
    unsigned char c = l % 32;
    if (c >= 26) c += '2' - 26;
    else c += 'A';
    buf[nbuf-i] = c;
    l /= 32;
  }
  for (i=n; i<8; i++)
    buf[nbuf++] = '=';
  return nbuf;
}

static size_t _tobase2n (ulongestint li, int bs, char *buf) {
  static const char digits[] = "0123456789ABCDEF";
  int mask = (1<<bs) - 1;
  char *s = buf;
  unsigned long rem;
  size_t nbuf;
  if (li == 0) {
    *buf = '0';
    return 1;
  }
  while (li) {
    rem = li & mask;
    li >>= bs;
    *s++ = digits[rem];
  }
  nbuf = s - buf;
  while (--s > buf) {
    char c = *buf;
    *buf++ = *s;
    *s = c;
  }
  return nbuf;
}

static size_t _tobase (ulongestint li, int base, char *buf) {
  ulongestint quot;
  unsigned long rem;
  char *s;
  size_t nbuf;
  if (li == 0) {
    *buf = '0';
    return 1;
  }
  s = buf;
  quot = li;
  while (quot != 0) {
    rem = quot % base;
    quot /= base;
    if (rem >= 36)
      *s++ = 'a' - 36 + rem;
    else if (rem >= 10)
      *s++ = 'A' - 10 + rem;
    else
      *s++ = '0' + rem;
  }
  nbuf = s - buf;
  while (--s > buf) {
    char c = *buf;
    *buf++ = *s;
    *s = c;
  }
  return nbuf;
}

static int largeinttostring (lua_State *L) {
  char buf[64];
  size_t buflen;
  largeinteger_t li = getlargeint(L, 1);
  int base = luaL_optint(L, 2, 0);
  int pad = luaL_optint(L, 3, 0);
  if (base == 0) {
    char *s = buf;
    buflen = 2;
    if (li < 0) {
      *s++ = '-';
      li = -li;
      buflen++;
    }
    s[0] = '0';
    s[1] = 'x';
    buflen += _tobase2n(li, 4, s+2);
  }
  else if (base == 85) {
    buflen = _tobase85(li, buf);
    pad = 0;
  }
  else if (base == 64) {
    buflen = _tobase64(li, buf);
    pad = 0;
  }
  else if (base == 32) {
    buflen = _tobase32(li, buf);
    pad = 0;
  }
  else if (base == 16)
    buflen = _tobase2n(li, 4, buf);
  else if (base == 8)
    buflen = _tobase2n(li, 3, buf);
  else if (base == 4)
    buflen = _tobase2n(li, 2, buf);
  else if (base == 2)
    buflen = _tobase2n(li, 1, buf);
  else if (base >= 2 && base <= 62)
    buflen = _tobase(li, base, buf);
  else
    return luaL_argerror(L, 2, "invalid base");
  lua_pushlstring(L, buf, buflen);
  if (pad > buflen) {
    int n = lua_gettop(L);
    pad -= buflen;
    if (pad > 64) {
      memset(buf, '0', 64);
      lua_pushlstring(L, buf, 64);
      lua_insert(L, -2);
      pad -= 64;
      while (pad > 64) {
        lua_pushvalue(L, -2);
        lua_insert(L, -2);
      }
    }
    if (pad > 0) {
      memset(buf, '0', pad);
      lua_pushlstring(L, buf, pad);
      lua_insert(L, -2);
    }
    lua_concat(L, lua_gettop(L) - n + 1);
  }
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
  a %= b;
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


static int b_packvli (lua_State *L) {
  luaL_Buffer b;
  ulongestint ul;
  largeinteger_t li = getlargeint(L, 1);
  if (li > LLONG_MAX/2 || li < 0/*LLONG_MIN/2*/)
    return luaL_argerror(L, 1, "integer out of range");
  luaL_buffinit(L, &b);
  ul = (ulongestint)li & (ULLONG_MAX/2);
  while (ul >= 0x80) {
    luaL_addchar(&b, ((unsigned char)ul)|0x80);
    ul >>= 7;
  }
  luaL_addchar(&b, (unsigned char)ul);
  luaL_pushresult(&b);
  return 1;
}

static int b_unpackvli (lua_State *L) {
  ulongestint ul;
  size_t nb;
  int argt;
  lua_settop(L, 1);
  argt = lua_type(L, 1);
  if (argt == LUA_TFUNCTION || argt == LUA_TUSERDATA || argt == LUA_TTABLE) {
    size_t l;
    const char *s;
    unsigned char c;
    ul = 0;
    nb = 0;
    lua_pushvalue(L, 1);
    lua_pushinteger(L, 1);
    lua_call(L, 1, 1);
    if (lua_isnil(L, -1)) {
      lua_pushinteger(L, 0);
      return 2;
    }
    s = lua_tolstring(L, -1, &l);
    if (s == NULL || l == 0)
      return luaL_error(L, "unterminated long integer");
    c = *s;
    lua_pop(L, 1);
    nb++;
    ul = c & 0x7F;
    while (c & 0x80) {
      lua_pushvalue(L, 1);
      lua_pushinteger(L, 1);
      lua_call(L, 1, 1);
      s = lua_tolstring(L, -1, &l);
      if (s == NULL || l == 0 || nb++ >= 9)
        return luaL_error(L, "unterminated long integer");
      c = *s;
      lua_pop(L, 1);
      ul |= (ulongestint)(c & 0x7F) << (nb * 7);
    }
  }
  else {
    size_t l;
    const char *s = luaL_checklstring(L, 1, &l);
    if (l == 0)
      return luaL_error(L, "unterminated long integer");
    if (l > 9)
      l = 9;
    ul = 0;
    nb = 0;
    ul = (unsigned char)s[0] & 0x7F;
    while (s[nb++] & 0x80) {
      if (nb >= l || s[nb] == 0)
        return luaL_error(L, "unterminated long integer");
      ul |= (ulongestint)((unsigned char)s[nb] & 0x7F) << (nb * 7);
    }
  }
  /*ul |= (ul<<1) & (ULLONG_MAX/2 + 1);*/
  if (ul > LONGESTMAX)
    newlargeint(L, (longestint)ul);
  else
    lua_pushnumber(L, (lua_Number)(longestint)ul);
  lua_pushinteger(L, nb);
  return 2;
}


static int b_packmbi (lua_State *L) {
  luaL_Buffer b;
  size_t n;
  ulongestint ul;
  ulongestint li = getlargeint(L, 1);
  luaL_buffinit(L, &b);
  n = 0;
  ul = li;
  while (ul > 0x7F) {
    n++;
	ul >>= 7;
  }
  luaL_addchar(&b, (unsigned char)(0xFF << (8-n)) | (ul >> n));
  while (n-- > 0) {
    luaL_addchar(&b, (unsigned char)li);
    li >>= 8;
  }
  luaL_pushresult(&b);
  return 1;
}

static int b_unpackmbi (lua_State *L) {
  ulongestint ul;
  size_t nb;
  int argt;
  lua_settop(L, 1);
  argt = lua_type(L, 1);
  if (argt == LUA_TFUNCTION || argt == LUA_TUSERDATA || argt == LUA_TTABLE) {
    size_t l;
    const char *s;
    unsigned char c;
    ul = 0;
    nb = 0;
    lua_pushvalue(L, 1);
    lua_pushinteger(L, 1);
    lua_call(L, 1, 1);
    if (lua_isnil(L, -1)) {
      lua_pushinteger(L, 0);
      return 2;
    }
    s = lua_tolstring(L, -1, &l);
    if (s == NULL || l == 0)
      return luaL_error(L, "unterminated long integer");
    c = *s;
    lua_pop(L, 1);
    while (c & 0x80) {
      nb++;
      c <<= 1;
    }
    ul = (ulongestint)c << (nb*7);
    if (nb > 0) {
      lua_pushvalue(L, 1);
      lua_pushinteger(L, nb);
      lua_call(L, 1, 1);
      s = lua_tolstring(L, -1, &l);
      if (s == NULL || l < nb)
        return luaL_error(L, "unterminated long integer");
      for (l=0; l<nb; l++) {
        ul |= (ulongestint)(unsigned char)s[l] << (8*l);
      }
      lua_pop(L, 1);
    }
    nb++;
  }
  else {
    size_t l;
    const char *s = luaL_checklstring(L, 1, &l);
    unsigned char c;
    if (l == 0)
      return luaL_error(L, "unterminated long integer");
    ul = 0;
    nb = 0;
    c = *s;
    while (c & 0x80) {
      nb++;
      c <<= 1;
    }
    ul = (ulongestint)c << (nb*7);
    if (nb >= l)
      return luaL_error(L, "unterminated long integer");
    for (l=0; l<nb; l++) {
      ul |= (ulongestint)(unsigned char)s[l+1] << (8*l);
    }
    nb++;
  }
  if ((longestint)ul > LONGESTMAX || (longestint)ul < LONGESTMIN)
    newlargeint(L, (longestint)ul);
  else
    lua_pushnumber(L, (lua_Number)(longestint)ul);
  lua_pushinteger(L, nb);
  return 2;
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
  {"tostring", largeinttostring},
  {NULL, NULL}
};

static const struct luaL_reg thislib[] = {
  {"pack", b_pack},
  {"unpack", b_unpack},
  {"largeinteger", b_largeint},
  {"packvli", b_packvli},
  {"unpackvli", b_unpackvli},
  {"packmbi", b_packmbi},
  {"unpackmbi", b_unpackmbi},
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


