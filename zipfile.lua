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
Archive{
  __metatable = { __index = methods, __newindex = error }
  archivestart = number,
  firstentry = number,
  nextentry = number,
  numentries = number,
  entriesread = number,
  firstfile = { next=... },
  lastfile = file,
  names = { name=file }
  comment
}
header{
  createdversion,
  createdsystem,
  extractversion,
  flags,
  method,
  datetime,
  crc32
  compressedsize,
  uncompressedsize,
  name,
  istext,
  extattributes,
  extra = { field ... },
  comment,
}
]===]

local error,assert,type = error,assert,type
local setmetatable = setmetatable
local rawset,rawget = rawset,rawget
local require = require
local iopen = io.open
local strsub,concat = string.sub,table.concat
local gsub,strfind,strlower = string.gsub,string.find,string.lower
local int,modf = math.floor,math.modf
local tostring = tostring

local struct = require"larc.struct"
local strunpack = struct.unpack

-- Get the platform-specific directory separator.
local path_sep = strsub(package.config, 1, strfind(package.config,"\n")-1)

module"larc.zipfile"

--[======================================================[
    Handlers for supported compression methods.
--]======================================================]

--[[No compression, just store.
  ]]
local function stored_handler(data)
  return data
end
local function stored_compress()
  return stored_handler
end

--[[ZLib deflate compression.
  ]]
local function zlib_decompress(zip)
  local zlib = require"larc.zlib"
  return zlib.decompressor{wbits=-15}
end
local function zlib_compress(zip)
  local zlib = require"larc.zlib"
  return zlib.compressor{level=9, wbits=-15}
end

--[[Bzip2 compression.
  ]]
local function bzip2_decompress(zip)
  local bz2 = require"larc.bzip2"
  return bz2.decompressor()
end
local function bz2_compress(zip)
  local bz2 = require"larc.bzip2"
  return bz2.compressor{blocksize=9}
end

--[[LZMA compression.
  ]]
local function lzma_decompress(zip)
  local lzma = require"larc.lzma"
  local ver,sz = strunpack("<H2", zip._handle:read(4))
  local filter = lzma.filter("lzma1", zip._handle:read(sz))
  return lzma.decompressor{format="raw", filter=filter}
end
local function lzma_compress(zip)
  local lzma = require"larc.lzma"
  local filter = lzma.filter("lzma1")
  local prop = tostring(filter)
  zip._handle:write("\4\65", strpack("<H",#prop), prop)
  return lzma.compressor{format="raw",filter=filter}
end

--[[Table of compression factories.
  ]]
local decompress_engine = {
  [0] = stored_compress;
  [8] = zlib_decompress;
--[9] = deflate64_decompress;
  [12] = bzip2_decompress;
  [14] = lzma_decompress;
--[19] = lz77_decompress;
--[98] = ppmd_decompress;
}

local compress_engine = {
  [0] = stored_compress;
  [12] = bzip2_compress;
  [14] = lzma_compress;
}

--[[When set, names in the archive are set to lower-case.
    Must be set prior to creating the zipfile object, and 
    will persist for the life of the object.
  ]]
local ignore_case = false

--[[ Find the last occurance of s2 in s1.
  ]]
local function strfindlast(s1, s2)
  local p1, p2 = (strfind(s1, s2, 1, true))
  while p1 do
    p2 = p1
    p1 = strfind(s1, s2, p1+1, true)
  end
  return p2
end

local function checkflags(flags, ...)
  local function _isset(b, ...)
    if b then
      local m,f = modf(flags / 2^(b+1))
      if f >= 0.5 then
        return true
      end
      return _isset(...)
    end
    return false
  end
  return _isset(...)
end

--[[Create table of components from DOS date and time.
  ]]
local function convertdatetime(zipdate, ziptime)
  return {
    year = (int(zipdate / 512) % 128) + 1980;
    month = (int(zipdate / 32) % 16) - 1;
    day = zipdate % 32;
    hour = int(ziptime / 2048) % 32;
    min = int(ziptime / 32) % 64;
    sec = int(ziptime % 32) * 2;
  }
end

--[[Get DOS attributes (as string) and *ix mode.
  ]]
local function convertattributes(mode)
  local attr = ""
  local m,f = modf(mode/2)
  if f >= 0.5 then attr = attr..'R' end
  m,f = modf(m/2)
  if f >= 0.5 then attr = attr..'H' end
  m,f = modf(m/2)
  if f >= 0.5 then attr = attr..'S' end
  m,f = modf(m/2)
  if f >= 0.5 then attr = attr..'V' end
  m,f = modf(m/2)
  if f >= 0.5 then attr = attr..'D' end
  m,f = modf(m/2)
  if f >= 0.5 then attr = attr..'A' end
  return attr,int(m/1024)
end

--[[Change slashes in the path to the platform-specific separator.
    Also removes leading and trailing slashes.
  ]]
local function convertpath(name)
  -- trims leading and trailing slashes
  name = gsub(name, "^/*(.*/?[^/]+)/*$", "%1")
  return gsub(name, "/+", path_sep)
end

local function zip_mt_newindex()
  error("zipfile objects are read-only")
end

local function zip_fileinfo(file)
  local attr,mode = convertattributes(file.attributes)
  return {
    name = file.name;
    size = file.uncompressedsize;
    time = convertdatetime(file.dosdate, file.dostime);
    attributes = attr;
    mode = mode;
    crc32 = file.crc32;
  }
end

--[[Methods for reading a file in a Zip.
  ]]
local zipfile = {}

function zipfile:close()
end

function zipfile:read(size)
  if size <= 0 then
    return ""
  end
  -- Lua can't duplicate a handle, so the file 
  -- position has to be coordinated manually.
  local handle, engine = self._handle, self._engine
  local buffer = { }
  local outbuf = self._buffer
  local buflen = #outbuf
  handle:seek("set",self._fpos)
  while buflen < size and self._inbytes < self._maxinbytes do
    buffer[#buffer+1] = outbuf
    local inbuf = handle:read(1024)
    local inbytes = #inbuf
    if self._inbytes + inbytes > self._maxinbytes then
      inbytes = self._maxinbytes - self._inbytes
      inbuf = strsub(inbuf, 1, inbytes)
    end
    self._inbytes = self._inbytes + inbytes
    self._fpos = self._fpos + inbytes
    local errmsg,errnum
    outbuf,errmsg,errnum = engine(inbuf)
    assert(errnum >= 0, "decompression error", errnum)
    buflen = buflen + #outbuf
  end
  if buflen == 0 then
    return nil
  end
  local pos = size - buflen
  self._buffer = strsub(outbuf, #outbuf+pos+1)
  buffer[#buffer+1] = strsub(outbuf, 1, #outbuf+pos)
  return concat(buffer)
end

local zipfile_mt = { __index=zipfile, __newindex=zip_mt_newindex }

local function zipfile_open(zip, file, engine, compressedsize, uncompressedsize)
  file._handle = zip._handle
  file._fpos = zip._handle:seek()
  file._engine = engine
  file._maxinbytes = compressedsize
  file._maxoutbytes = uncompressedsize
  file._inbytes = 0
  file._outbytes = 0
  file._buffer = ""
  return setmetatable(file, zipfile_mt)
end

--[[Read a Zip archive central directory that begins at endpos.
    This assumes that the signature has already been verified.
  ]]
local function readcentraldirectory(zip, endpos)
  -- disk number, cdir disk number, 
  -- number of disk cdir entries, total number of cdir entries,
  -- cdir size, cdir start, text size
  zip._handle:seek("set", endpos)
  local dn,cdn,cde,cen,csz,cst,tsz = 
      strunpack("<x4H4L2H", zip._handle:read(22))
  if not tsz then
    return false
  end
  zip._archivestart = endpos - (cst + csz)
  if zip._archivestart < 0 then
    return false
  end
  if cde ~= cen or dn ~= 0 or cdn ~= 0 then
    return false
  end
  if tsz > 0 then
    zip.comment = zip._handle:read(tsz)
  end
  zip._firstentry = cst
  zip._nextentry = cst
  zip._numentries = cen
  zip._entriesread = 0
  zip._names = {}
  return true
end

--[[Scan a file for the end-of-directory record.
  ]]
local function findcentraldirectory(zip)
  -- Skip the last 18 bytes of the file because the
  -- record needs to be at least that long. Then 
  -- read 256 bytes at a time backwards until the 
  -- signature is found.
  local pos = zip._handle:seek("end",-274)
  local backmax, limit = 0,-1
  if pos then
    backmax = pos - 65283
    if backmax > 255 then
      limit = backmax - 255
    end
  else
    pos = zip._handle:seek("set",0)
  end
  local carry = ""
  while pos and pos >= limit do
    local block = zip._handle:read(256) .. carry
    local start = strfindlast(block, "PK\5\6")
    if start then
      pos = pos + start - 1
      if pos < backmax then
        return false
      end
      return readcentraldirectory(zip, pos)
    end
    if pos == 0 then
      break
    end
    carry = strsub(block,1,3)
    pos = pos - 256
    if pos < 0 then
      pos = 0
    end
    pos = zip._handle:seek("set", pos)
  end
  return false
end

--[[Get the next unread entry from the central directory.
  ]]
local function readnextentry(zip)
  if zip._entriesread == zip._numentries then
    return nil
  end
  zip._handle:seek("set", zip._archivestart + zip._nextentry)
  -- creator version, creator system, extract version, GP flags,
  -- method, time, date, crc32, compressed size, uncompressed size,
  -- name length, extra length, text length, disk number,
  -- internal attributes, external attributes, header start
  local sig,cv,cs,ev,gp,cm,tm,dt,crc,csz,usz,nsz,xsz,tsz,dn,ia,ea,st = 
      strunpack("<c4BBH5L3H5L2", zip._handle:read(46))
  assert(sig=="PK\1\2", "error in Zip file, expected central file header")
  zip._nextentry = zip._nextentry + nsz + xsz + tsz + 46
  zip._entriesread = zip._entriesread + 1
  local file = {
    createdversion = cv,
    createdsystem = cs,
    extractversion = ev,
    flags = gp,
    method = cm,
    dostime = tm,
    dosdate = dt,
    crc32 = crc,
    compressedsize = csz,
    uncompressedsize = usz,
    internal = ia,
    attributes = ea,
    headerstart = st
  }
  file.name = assert(zip._handle:read(nsz))
  if xsz > 0 then
    --file.extra = readextrafields(xsz)
    zip._handle:seek("cur",xsz)
  end
  if tsz > 0 then
    file.comment = zip.handle:read(tsz)
  end
  if zip._case then
    zip._names[strlower(file.name)] = file
  else
    zip._names[file.name] = file
  end
  if zip._lastfile then
    zip._lastfile.next = file
    zip._lastfile = file
  else
    rawset(zip,'_firstfile',file)
    rawset(zip,'_lastfile',file)
  end
  return file
end

--[[Method table for zip files.
  ]]
local zip = {}

--[[Get the entry for a file.
  ]]
local function zip_getfile(zip, name)
  assert(type(name)=='string')
  local file
  if zip._case then
    name = strlower(name)
  end
  file = zip._names[name]
  if not file and zip._entriesread < zip._numentries then
    local nextfile = readnextentry(zip)
    while nextfile do
      if zip._case then
        if name == strlower(nextfile.name) then
          file = nextfile
          break
        end
      else
        if name == nextfile.name then
          file = nextfile
          break
        end
      end
      nextfile = readnextentry(zip)
    end
  end
  if not file then
    return nil, name.." not in archive"
  end
  return file
end

local function seektofile(zip, file, enginetable)
  local pos, message = zip._handle:seek("set", file.headerstart)
  if not pos then
    return nil, message
  end
  local sig,ev,gp,cm,crc,csz,usz,nsz,xsz = 
      strunpack("<c4H3x4L3H2", zip._handle:read(30))
  if not xsz then
    return nil, "read error"
  end
  assert(sig=="PK\3\4", "error in Zip file, expected file header")
  local name = zip._handle:read(nsz)
  assert(name==file.name, "error in Zip file, file header mismatch")
  zip._handle:seek("cur",xsz)
  local engine = enginetable[cm]
  if not engine then
    return nil, "unknown compression type ("..cm..")"
  end
  return usz,csz,engine
end

--[[Decompress and save a file.
    Without dest, saves to the current directory using the
    file name from the archive, including sub-directories.
    Without name or dest, extracts all files to the current
    directory.
  ]]
function zip:extract(name, dest)
  if dest then
    local t = strsub(dest, -1)
    if t ~= '/' or t ~= path_sep then
      dest = dest .. path_sep
    end
  else
    dest = ""
  end
  if name then
    local file, message = zip_getfile(self, name)
    if not file then
      return false, message
    end
    if checkflags(file.attributes,4,3) then
      return false, name.." is not a file"
    end
    local path = convertpath(file.name)
    local usz,csz,engine = assert(seektofile(self, file, decompress_engine))
    local fhandle, message = iopen(dest..path, "wb")
    if not fhandle then
      return nil, message
    end
    if csz ~= 0 then
      engine = assert(engine(self))
      fhandle:write(engine(self._handle:read(csz)))
    end
    fhandle:close()
  else
    local file
    repeat
      file = readnextentry(self)
    until not file
    file = self._firstfile
    while file do
      if not checkflags(file.attributes,4,3) then
        local path = convertpath(file.name)
        local usz,csz,engine = assert(seektofile(self, file, decompress_engine))
        local fhandle = iopen(dest..path, "wb")
        if csz ~= 0 then
          engine = assert(engine(self))
          fhandle:write(engine(self._handle:read(csz)))
        end
        fhandle:close()
      end
      file = file.next
    end
  end
  return true
end

function zip:getinfo(name)
  local file, message = zip_getfile(self, name)
  if not file then
    return nil, message
  end
  return zip_fileinfo(file)
end

function zip:read(name, size)
  local file, message = assert(zip_getfile(self, name))
  local usz,csz,engine = assert(seektofile(self, file, decompress_engine))
  if usz == 0 then
    return ""
  end
  engine = assert(engine(self))
  if not size or size < usz then
    size = usz
  end
  return engine(assert(self._handle:read(csz)))
end

function zip:open(name)
  local file, message = zip_getfile(self, name)
  if not file then
    return nil, message
  end
  local usz,csz,engine = seektofile(self, file, decompress_engine)
  if not usz then
    return nil,csz -- actually error message
  end
  engine, message = engine(self)
  if not engine then
    return nil, message
  end
  return zipfile_open(self, zip_fileinfo(file), engine, csz, usz)
end

function zip:names()
  local file = self._firstfile
  return function(self)
      if not file then
        file = readnextentry(self)
        if not file then
          return nil
        end
      end
      local name = file.name
      file = file.next
      return name
    end, self
end

function zip:close()
  if self._ownhandle then
    self._handle:close()
    self._ownhandle = nil
  end
end

local zip_mt = { __index=zip, __newindex=zip_mt_newindex }

local function zip_open(handle, ownhandle)
  local zip = { _handle = handle, _ownhandle = ownhandle, _case = ignore_case }
  if not findcentraldirectory(zip) then
    return nil
  end
  return setmetatable(zip, zip_mt)
end

function set_case_sensitive(option)
  -- coerces to boolean, and also reverses meaning
  ignore_case = not option
end

function open(file, mode)
  mode = mode or "r"
  do
    local tn = type(file)
    assert(tn=='string' or ((
        tn=='table' or tn=='userdata') and type(file.seek)=='function'))
    assert(type(mode)=='string')
  end
  if mode ~= "r" then
    error("invalid mode '"..mode.."' (zip files can only be opened for reading)")
  end
  local handle, message, ownhandle
  if type(file)=='string' then
    handle, message = iopen(file, "rb")
    if not handle then
      return nil, message
    end
    ownhandle = true
  else
    handle = file
  end
  zip = zip_open(handle, ownhandle)
  if not zip then
    return nil, "Not a valid Zip archive"
  end
  return zip
end
