# LArc library
# Copyright (C) 2010 Tom N Harris. All rights reserved.
#
#  This software is provided 'as-is', without any express or implied
#  warranty.  In no event will the authors be held liable for any damages
#  arising from the use of this software.
#
#  Permission is granted to anyone to use this software for any purpose,
#  including commercial applications, and to alter it and redistribute it
#  freely, subject to the following restrictions:
#
#  1. The origin of this software must not be misrepresented; you must not
#     claim that you wrote the original software. If you use this software
#     in a product, an acknowledgment in the product documentation would be
#     appreciated but is not required.
#  2. Altered source versions must be plainly marked as such, and must not be
#     misrepresented as being the original software.
#  3. This notice may not be removed or altered from any source distribution.
#  4. Neither the names of the authors nor the names of any of the software 
#     contributors may be used to endorse or promote products derived from 
#     this software without specific prior written permission.

# change these to reflect your Lua installation
LUA= /local
LUAINC= $(LUA)/include
LUALIB= $(LUA)/lib
LUABIN= $(LUA)/bin
ZLIB= ../../Projects/zlib-1.2.3
ZLIBINC= -I$(ZLIB)
ZLIBLIB= $(ZLIB)/libz.a
BZ2= ../../Projects/bzip2-1.0.5
BZ2INC= -I$(BZ2)
BZ2LIB= $(BZ2)/libbz2.a
LZMA= ../../Projects/xz-4.999.9beta_20091209
LZMAINC= -I$(LZMA)/src/liblzma/api
LZMALIB= $(LZMA)/src/liblzma/.libs/liblzma.a

#PLAT=unix
#PLAT=cygwin
PLAT=mingw32
#PLAT=darwin
ifndef PLAT
PLAT= $(error Please define PLAT)
endif

V= 0.0

# probably no need to change anything below here
CC= gcc
CFLAGS= $(INCS) $(WARN) -O2 $(G)
WARN= -Wall
INCS= -I$(LUAINC) $(ZLIBINC) $(BZ2INC) $(LZMAINC)
LIBS= 
MAKESO= $(CC) $(G) -shared
ifeq ($(PLAT),mingw32)
LIBS+= $(LUABIN)/lua51.dll
S= dll
else ifeq ($(PLAT),cygwin)
LIBS+= $(LUABIN)/lua51.dll
S= so
else
S= so
endif
ifeq ($(PLAT),darwin)
MAKESO= env MACOSX_DEPLOYMENT_TARGET=10.3 $(CC) -bundle -undefined dynamic lookup
endif

CP= install -m 0644
CPEXE= install -m 0755
MKDIR= install -m 0755 -d

MODULEPATH= $(LUALIB)/larc
LMODULES= gzfile.lua bz2file.lua tarfile.lua zipfile.lua ziploader.lua
CMODULES= struct.$(S) zlib.$(S) bzip2.$(S) lzma.$(S)

SRCS= struct.c lzlib.c lbzip2.c llzma.c
OBJS= struct.o lzlib.o lbzip2.o llzma.o

EXTRA= lua-archive.txt lua-archive-0.0-0.rockspec

all: so

o: $(OBJS)

so: $(CMODULES)

dll: $(CMODULES)

struct.$(S): struct.o
	$(MAKESO) -o $@ struct.o $(LIBS)

zlib.$(S): lzlib.o
	$(MAKESO) -o $@ lzlib.o $(ZLIBLIB) $(LIBS)

bzip2.$(S): lbzip2.o
	$(MAKESO) -o $@ lbzip2.o $(BZ2LIB) $(LIBS)

lzma.$(S): llzma.o
	$(MAKESO) -o $@ llzma.o $(LZMALIB) $(LIBS)

lzlib.o: lzlib.c shared.h
lbzip2.o: lbzip2.c shared.h
llzma.o: llzma.c shared.h

clean:
	rm -f $(OBJS) core core.*

install: $(CMODULES) $(LMODULES)
	$(MKDIR) $(MODULEPATH)
	$(CPEXE) $(CMODULES) $(MODULEPATH)
	$(CP) $(LMODULES) $(MODULEPATH)

dist: $(SRCS) $(LMODULES) $(EXTRA) Makefile
	mkdir -p ./dist/lua-archive-$(V)
	cp $^ ./dist/lua-archive-$(V)
	tar -cvjf lua-archive-$(V).tar.bz2 -C ./dist lua-archive-$(V)
	rm -rf ./dist
