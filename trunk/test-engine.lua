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
compr,used,status = assert(compress(hello))
assert(used==#hello and status>=0)
uncompr,used,status = assert(decompress(compr))
assert(used==#compr and status>=0)
assert(uncompr==hello)
print("OK!")

deflate = assert(compressor{level=0})
compr = ""
for i=1,#hello do
  compr = compr .. assert(deflate(hello:sub(i,i)))
end
compr = compr .. assert(deflate())
inflate = assert(decompressor())
uncompr = ""
for i=1,#compr do
  uncompr = uncompr .. assert(inflate(compr:sub(i,i)))
end
assert(uncompr==hello)
print("OK!")
