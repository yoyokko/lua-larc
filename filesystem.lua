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

local assert,error,pcall = assert,error,pcall
local require,type = require,type
local select = select
local sub,gsub,find = string.sub,string.gsub,string.find
local concat = table.concat
local iopen = io.open

-- Get the platform-specific directory separator.
local sep = sub(package.config, 1, find(package.config,"\n")-1)

module "larc.filesystem"

local truefunc = function() return true end
local falsefunc = function() return false end

local lfs
local function checklfs()
  lfs = require "lfs"
  checklfs = truefunc
  return true
end

local posix
local function checkposix()
  local success,_posix = pcall(require, "posix")
  if success then
    posix = _posix
    checklfs = truefunc
  else
    checkposix = falsefunc
  end
  return success
end

--[[ Find the last occurance of s2 in s1.
  ]]
local function findlast(s1, s2, p)
  local p1, p2 = (find(s1, s2, p or 1, true))
  while p1 do
    p2 = p1
    p1 = find(s1, s2, p1+1, true)
  end
  return p2
end

-- Export director separator.
path_sep = sep

--[[Change slashes in the path to the platform-specific separator.
  ]]
function removeslashes(name)
  return gsub(name, "/", sep)
end

--[[Change platform-specific separator to slashes.
  ]]
function addslashes(name)
  return gsub(name, sep, "/")
end

--[[Removes leading and trailing slashes.
    Also converts slashes to platform-specific separator,
    since this function is usually used with paths from
    external sources that are likely slash-delimited.
  ]]
local dblsep = sep..sep
local dupsepre = sep.."+"
local leadsepre = "^"..sep.."*(.*)$"
function normalize(name, isabs)
  local root = ""
  name = gsub(name, "/", sep)
  if isabs then
    if sub(name,1,2) == dblsep then
      root = dblsep
    elseif sub(name,1,1) == sep then
      root = sep
    end
  end
  name = gsub(name, leadsepre, "%1")
  return root .. gsub(name, dupsepre, sep)
end

function joinpath(...)
  local n = select('#', ...)
  if n == 0 then
    return ""
  end
  local root = 1
  for i = n,1,-1 do
    if sub(select(i,...), 1, 1) == sep then
      root = i
      break
    end
  end
  local path = {}
  for i = root,n-1 do
    local part = select(i, ...)
    if part ~= "" then
      path[#path+1] = part
      if sub(part, -1) ~= sep then
        path[#path+1] = sep
      end
    end
  end
  path[#path+1] = select(n, ...)
  return concat(path)
end

function splitpath(name)
  local pos = findlast(name, sep)
  if not pos then
    return "", name
  end
  return sub(name, 1, pos-1), sub(name, pos+1)
end

function dirname(name)
  local pos = findlast(name, sep)
  if not pos then
    return ""
  end
  if pos == 1 then
    return sep
  end
  return sub(name, 1, pos-1)
end

function basename(name)
  local pos = findlast(name, sep)
  if not pos then
    return name
  end
  return sub(name, pos+1)
end

function splitext(name)
  local extpos = findlast(name, '.', findlast(name, sep))
  if not extpos then
    return name, ""
  end
  return sub(name, 1, pos-1), sub(name, pos)
end

function makedirs(path)
  checklfs()
  local dir,name = splitpath(path)
  local mode
  if dir ~= "" then
    mode = lfs.attributes(dir, 'mode')
    if not mode then
      makedirs(dir)
    end
  end
  mode = lfs.attributes(path)
  if not mode then
    return lfs.mkdir(path)
  else
    if mode ~= "directory" then
      return false, "\""..name.."\" already exists and is not a directory"
    end
  end
  return true
end

function makedirstofile(name)
  return makedirs(dirname(name))
end

local function fake(what, name, dest)
  local link, message = iopen(name, 'w')
  if not link then
    return nil,message
  end
  link:write('!<',what,'>', dest)
  link:close()
  return true
end

function makelink(name, dest)
  if not checkposix() then
    return fake('link', name, dest)
  end
  return posix.link(dest, name)
end

function makesymlink(name, dest)
  if not checkposix() then
    return fake('symlink', name, dest)
  end
  return posix.symlink(dest, name)
end

function makepipe(name)
  if not checkposix() then
    return fake('fifo', name, "")
  end
  return posix.mkfifo(name)
end

function makepipe(name)
  if not checkposix() then
    return fake('fifo', name, "")
  end
  return posix.mkfifo(name)
end

function makedevice(name, what, major, minor)
  if what == 'c' then
    what = "character"
  elseif what == 'b' then
    what = "block"
  end
  return fake(what, name, major..":"..minor)
end

function isposix()
  return checkposix()
end

function exists(name)
  checklfs()
  local mode = lfs.attributes(name)
  return not not mode
end

function isdir(name)
  checklfs()
  local mode = lfs.attributes(name)
  return mode == 'directory'
end

function isfile(name)
  checklfs()
  local mode = lfs.attributes(name)
  return mode == 'file'
end

return _M
