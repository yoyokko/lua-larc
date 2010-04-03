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
--[===[
  Package loader for zip archives.
  
  Require this module and you will be able to load Lua libraries
  that are stored in zip archives. Add your zip files to the
  ziploader.archives table.
--]===]

local gsub = string.gsub
local concat = table.concat
local ipairs = ipairs
local load = load

local zipfile = require"larc.zipfile"

local ziploader = {}

--[[Search for a Lua library in each zip archive 
    listed in ziploader.archives. If found, load 
    and return the chunk.
  ]]
local function zip_searcher(module)
  local name = gsub(module, '.', '/') .. '.lua'
  local errors = {}
  for _,archive in ipairs(ziploader.archives) do
    local zip,file,message
    zip,message = zipfile.open(archive)
    if not zip then
      errors[#errors+1] = "\n\tcannot read \""..archive.."\": "..message
    else
      file,message = zip:open(name)
      if file then
        return load(function() return file:read(1024) end, "@"..archive.."/"..name)
      end
      errors[#errors+1] = "\n\tno file \""..archive.."/"..name.."\""
    end
  end
  return concat(errors)
end
table.insert(package.loaders, 4, zip_searcher)

--[[List of zip archives that will be searched for libraries.
  ]]
ziploader.archives = {}

return ziploader
