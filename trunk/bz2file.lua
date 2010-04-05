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

local assert,error,type = assert,error,type
local tonumber,tostring = tonumber,tostring
local setmetatable = setmetatable
local sub,concat,strrep = string.sub,table.concat,string.rep
local find,match = string.find,string.match
local iopen = io.open

local bzip2 = require"larc.bzip2"

module"larc.bz2file"

--[[Method tables for bz2file.
  ]]
local bz2_reader = {}
local bz2_writer = {}

--[[Support the "*line" read argument.
  ]]
local function read_line(bz2)
  if bz2._eof and #bz2._buffer == 0 then
    return nil
  end
  local handle, process = bz2._handle, bz2._process
  local buffer = {}
  local outbuf = bz2._buffer
  local buflen = #outbuf
  local pos = find(outbuf, "\n", 1, true)
  while not pos and not bz2._eof do
    buffer[#buffer+1] = outbuf
    local inbuf = handle:read(1024)
    if not inbuf then
      bz2._eof = true
      break
    end
    local errmsg,errnum
    outbuf,errmsg,errnum = process(inbuf)
    assert(errnum>=0, errmsg, errnum)
    if errnum == bzip2.BZ_STREAM_END then
      bz2._eof = true
    end
    buflen = buflen + #outbuf
    pos = find(outbuf, "\n", 1, true)
  end
  if buflen == 0 then
    return nil
  end
  if pos then
    bz2._buffer = sub(outbuf, pos+1)
    bz2._pos = bz2._pos + buflen + pos - #outbuf
    buffer[#buffer+1] = sub(outbuf, 1, pos-1)
  else
    bz2._buffer = ""
    bz2._pos = bz2._pos + buflen
    buffer[#buffer+1] = outbuf
  end
  return concat(buffer)
end

--[[Support the "*all" read argument.
  ]]
local function read_all(bz2)
  if bz2._eof and #bz2._buffer == 0 then
    return ""
  end
  local handle, process = bz2._handle, bz2._process
  local buffer = { bz2._buffer }
  local buflen = #buffer[1]
  bz2._buffer = ""
  while not bz2._eof do
    local inbuf = handle:read(1024)
    if not inbuf then
      bz2._eof = true
      break
    end
    local outbuf,errmsg,errnum = process(inbuf)
    assert(errnum>=0, errmsg, errnum)
    if errnum == bzip2.BZ_STREAM_END then
      bz2._eof = true
    end
    buflen = buflen + #outbuf
    buffer[#buffer+1] = outbuf
  end
  bz2._pos = bz2._pos + buflen
  buffer = concat(buffer)
  return buffer
end

--[[Read up to ''size'' bytes from a bz2file.
  ]]
local function read_bytes(bz2, size)
  if bz2._eof and #bz2._buffer == 0 then
    return nil
  end
  if size <= 0 then
    return ""
  end
  local handle, process = bz2._handle, bz2._process
  local buffer = {}
  local outbuf = bz2._buffer
  local buflen = #outbuf
  while buflen < size and not bz2._eof do
    buffer[#buffer+1] = outbuf
    local inbuf = handle:read(1024)
    if not inbuf then
      bz2._eof = true
      break
    end
    local errmsg,errnum
    outbuf,errmsg,errnum = process(inbuf)
    assert(errnum>=0, errmsg, errnum)
    if errnum == bzip2.BZ_STREAM_END then
      bz2._eof = true
    end
    buflen = buflen + #outbuf
  end
  if buflen == 0 then
    return nil
  end
  local pos = size - buflen
  bz2._buffer = sub(outbuf, #outbuf+pos+1)
  bz2._pos = bz2._pos + buflen + pos
  buffer[#buffer+1] = sub(outbuf, 1, #outbuf+pos)
  return concat(buffer)
end

--[[Standard file handle read method,
    but without "*number" conversion.
  ]]
function bz2_reader:read(size)
  assert(self._handle, "attempt to read from a closed file")
  if not size then
    return read_line(self)
  end
  if type(size) == 'string' then
    local star,tag = sub(size,1,1),sub(size,2,2)
    if star == '*' then
      if tag == 'l' then
        return read_line(self)
      end
      if tag == 'a' then
        return read_all(self)
      end
      error("invalid option")
    end
  end
  return read_bytes(self, tonumber(size))
end

--[[Skip ahead in the file.
    Basically the same as read_bytes but just 
    discards the bytes.
  ]]
local function read_skip(bz2, size)
  if size <= 0 or (bz2._eof and #bz2._buffer == 0) then
    return bz2._pos
  end
  local handle, process = bz2._handle, bz2._process
  local outbuf = bz2._buffer
  local bytesread = #outbuf
  while bytesread < size and not bz2._eof do
    local inbuf = handle:read(1024)
    if not inbuf then
      bz2._eof = true
      break
    end
    local errmsg,errnum
    outbuf,errmsg,errnum = process(inbuf)
    assert(errnum>=0, errmsg, errnum)
    if errnum == bzip2.BZ_STREAM_END then
      bz2._eof = true
    end
    bytesread = bytesread + #outbuf
  end
  -- pos is the offset from the end of the buffer
  local pos = size - bytesread
  bz2._buffer = sub(outbuf, #outbuf+pos+1)
  bz2._pos = bz2._pos + bytesread + pos
  return bz2._pos
end

--[[Rewind the file then skip to a new position.
    Fails if the underlying file handle doesn't 
    support seeking.
  ]]
local function read_rewind(bz2, newpos)
  -- Return to the start of the compressed stream.
  assert(bz2._bzstreamstart and
      bz2._handle:seek("set",bz2._bzstreamstart), "file handle cannot seek backwards")
  bz2._eof = false
  bz2._pos = 0
  bz2._buffer = ""
  bz2._process = bzip2.decompressor()
  return read_skip(bz2, newpos)
end

--[[Standard file handle seek method
    for a bz2file in read mode.
    Seeking forward is possible by decompressing 
    and discarding bytes. To seek backward, the 
    stream must be completely rewound and read from the 
    beginning.
  ]]
function bz2_reader:seek(whence, newpos)
  whence = whence or "cur"
  newpos = newpos or 0
  if whence == "end" then
    error("cannot seek from end of a bz2file")
  end
  if whence == "set" then
    newpos = newpos - self._pos
  end
  if newpos > 0 then
    return read_skip(self, newpos)
  elseif newpos < 0 then
    return read_rewind(self, self._pos + newpos)
  end
  -- if newpos == 0 then
  return self._pos
end

--[[Standard file handle close method
    for a bz2file in read mode.
  ]]
function bz2_reader:close()
  if self._ownhandle then
    self._handle:close()
    self._ownhandle = nil
  end
  self._handle = nil
  return true
end

--[[Convert values to strings and write to
    a bz2file.
  ]]
local function write_values(bz2, val, ...)
  if not val then
    return bz2
  end
  local outbuf,errnum = bz2._process(tostring(val))
  if errnum < 0 then
    return nil,errnum
  end
  local res,message = bz2._handle:write(outbuf)
  if not res then
    return res,message
  end
  bz2._size = bz2._size + #val
  return write_values(bz2, ...)
end

--[[Standard write method.
  ]]
function bz2_writer:write(...)
  assert(self._handle, "attempt to write to a closed file")
  return write_values(self, ...)
end

--[[Standard file handle seek.
    Can only seek forward while writing a gzfile.
  ]]
function bz2_writer:seek(whence, newpos)
  whence = whence or "cur"
  newpos = newpos or 0
  if whence == "set" then
    newpos = newpos - self._size
  end
  assert(newpos>=0, "attempt to seek backwards while writing a bz2file")
  if newpos > 1024 then
    local str = strrep('\0', 1024)
    while newpos >= 1024 do
      local outbuf,errmsg,errnum = self._process(str)
      if errnum < 0 then
        return nil,errmsg,errnum
      end
      local res,message = self._handle:write(outbuf)
      if not res then
        return nil,message
      end
      self._size = self._size + 1024
      newpos = newpos - 1024
    end
  end
  if newpos > 0 then
    local str = strrep('\0', newpos)
    local outbuf,errmsg,errnum = self._process(str)
    if errnum < 0 then
      return nil,errmsg,errnum
    end
    local res,message = self._handle:write(outbuf)
    if not res then
      return nil,message
    end
    self._size = self._size + newpos
  end
  return self._size
end

--[[Standard close method
    for a bz2file in write mode.
  ]]
function bz2_writer:close()
  if self._handle then
    local outbuf,errmsg,errnum = self._process(nil)
    if outbuf then
      self._handle:write(outbuf)
    end
    if self._ownhandle then
      self._handle:close()
      self._ownhandle = nil
    end
    self._handle = nil
    if errnum < 0 then
      return nil, errmsg, errnum
    end
  end
  return true
end

--[[Error function to prevent changes to a gzfile.
  ]]
local function bz2_mt_newindex()
  error("bz2file objects are read-only")
end

--[[gzfile metatables.
  ]]
local bz2_read_mt = { __index=bz2_reader, __newindex=bz2_mt_newindex }
local bz2_write_mt = { __index=bz2_writer, __newindex=bz2_mt_newindex }

--[[Open a bz2file in read mode.
    If ''ownhandle'' is set, then the file
    handle will be closed with the bz2file.
  ]]
local function bz2file_open(handle, ownhandle)
  local bz2 = { _handle=handle, _ownhandle=ownhandle }
  bz2._process = bzip2.decompressor()
  if handle.seek then -- Disregard if seeking isn't possible.
    bz2._bzstreamstart = handle:seek("cur",0)
  end
  local data,message,errnum = handle:read(10) -- the smallest bz2 stream
  if not data then
    return nil,message
  end
  data,message,errnum = bz2._process(data)
  if errnum < 0 then
    return nil,message
  end
  bz2._eof = errnum == bzip2.BZ_STREAM_END
  bz2._buffer = data
  bz2._pos = 0
  return setmetatable(bz2, bz2_read_mt)
end

--[[Create a bz2file in write mode.
    If ''ownhandle'' is set, then the file
    handle will be closed with the bz2file.
  ]]
local function bz2file_create(handle, ownhandle, level)
  local bz2 = {
    _handle=handle,
    _ownhandle=ownhandle,
    _size=0
  }
  bz2._process = bzip2.compressor{blocksize=level}
  return setmetatable(bz2, bz2_write_mt)
end

--[[Standard open function for bz2files.
    The mode is either "r" or "w" and may also 
    have "b". (A bz2file is always in binary mode.)
    When writing a bz2file, the ''level'' option
    is a number in the range [1,9].
  ]]
function open(file, mode, level)
  mode = mode or "r"
  do
    local tn = type(file)
    assert(tn=='string' or ((
        tn=='table' or tn=='userdata') and type(file.seek)=='function'))
    assert(type(mode)=='string')
  end
  if match(mode,'^b?rb?') then
    mode = "rb"
  elseif match(mode,'b?wb?') then
    mode = "wb"
    level = tonumber(level) or 6
    assert(level>0 and level<=9, "invalid compression level")
  else
    error("invalid mode")
  end
  local handle, message, ownhandle
  if type(file)=='string' then
    handle, message = iopen(file, mode)
    if not handle then
      return nil, message
    end
    ownhandle = true
  else
    handle = file
  end
  if mode == "rb" then
    return bz2file_open(handle, ownhandle)
  else
    return bz2file_create(handle, ownhandle, level)
  end
end
