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
require"larc.lzma"

hello = "hello, hello!"
print("lzma version", larc.lzma.LZMA_VERSION)
print("physical memory", larc.lzma.physmem())

compress,decompress = larc.lzma.compress,larc.lzma.decompress
compressor,decompressor = larc.lzma.compressor,larc.lzma.decompressor
dofile("test-engine.lua")

crc32 = larc.lzma.crc32(hello)
assert(crc32==0xB39ADC9B)
c = larc.lzma.crc32(nil)
for i=1,#hello do
  c = larc.lzma.crc32(c, hello:sub(i,i))
end
assert(c==crc32)
print("OK!")
crc64 = larc.lzma.crc64(hello)
assert(crc64:tostring(64)=="gRhDAn8w9Xw=")
c = larc.lzma.crc64(nil)
for i=1,#hello do
  c = larc.lzma.crc64(c, hello:sub(i,i))
end
assert(c==crc64)
print("OK!")
