--[==========================================================================[
   LArc library
   Copyright (C) 2010 Tom N Harris. All rights reserved.
  
    This software is provided 'as-is', without any express or implied
    warranty.  In no event will the authors be held liable for any damages
    arising from the use of this software.
  
    Permission is granted to anyone to use this software for any purpose,
    including commercial applications, and to alter it and redistribute it
    freely, subject to the following restrictions:
  
    1. The origin of this software must not be misrepresented; you must not
       claim that you wrote the original software. If you use this software
       in a product, an acknowledgment in the product documentation would be
       appreciated but is not required.
    2. Altered source versions must be plainly marked as such, and must not be
       misrepresented as being the original software.
    3. This notice may not be removed or altered from any source distribution.
    4. Neither the names of the authors nor the names of any of the software 
       contributors may be used to endorse or promote products derived from 
       this software without specific prior written permission.
--]==========================================================================]
require"larc.zlib"

hello = "hello, hello!"
print("zlib version", larc.zlib.ZLIB_VERSION)

compress,decompress = larc.zlib.compress,larc.zlib.decompress
compressor,decompressor = larc.zlib.compressor,larc.zlib.decompressor
dofile("test-engine.lua")

crc32 = larc.zlib.crc32(hello)
assert(crc32==0xB39ADC9B)
c = larc.zlib.crc32(nil)
for i=1,#hello do
  c = larc.zlib.crc32(c, hello:sub(i,i))
end
assert(c==crc32)
c = larc.zlib.crc32_combine(
    larc.zlib.crc32(hello:sub(1,-7)),
    larc.zlib.crc32(hello:sub(-6)), 6)
assert(c==crc32)
print("OK!")
adler32 = larc.zlib.adler32(hello)
assert(adler32==0x21700496)
a = larc.zlib.adler32(nil)
for i=1,#hello do
  a = larc.zlib.adler32(a, hello:sub(i,i))
end
assert(a==adler32)
a = larc.zlib.adler32_combine(
    larc.zlib.adler32(hello:sub(1,-7)),
    larc.zlib.adler32(hello:sub(-6)), 6)
assert(a==adler32)
print("OK!")
