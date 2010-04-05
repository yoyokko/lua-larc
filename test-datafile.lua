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

assert(f:read(100) == string.rep('0',100))
assert(f:read "*line" == "")
assert(f:read "*l" == string.rep('1',100))
assert(f:seek("cur",353) == 555)
assert(f:read(51) == string.rep('5',50).."\n")
assert(f:seek() == 606)
assert(f:seek("set",959) == 959)
assert(f:seek("cur",-50) == 909)
assert(f:read "*a" == string.rep('9',100).."\n")
assert(f:seek("set") == 0)
assert(f:read(100) == string.rep('0',100))
print("OK!")
