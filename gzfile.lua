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
local find,match,strbyte,strchar = string.find,string.match,string.byte,string.char
local iopen = io.open
local modf = math.modf
local unpack = unpack or table.unpack

local zlib = require"larc.zlib"

module"larc.gzfile"

--[[Table of OS bytes.
  ]]
local os_code = {
  [0] = "msdos";
  [1] = "amiga";
  [2] = "vms";
  [3] = "unix";
  [4] = "vm/cms";
  [5] = "atari";
  [6] = "os/2";
  [7] = "macos";
  [8] = "z-system";
  [9] = "cp/m";
  [10] = "tops-20";
  [11] = "windows";
  [12] = "qdos";
  [13] = "riscos";
  [15] = "prime";
}
-- Flip the table for reverse lookups.
for i=0,15 do
  if os_code[i] then
    os_code[os_code[i]] = i
  end
end

--[[Converts the packed bits in a number to boolean values.
    For each argument the state of the bit in that position 
    is returned as true (bit is set) or false. The least 
    significant bit is #0. Requires at least 2^-1 precision.
  ]]
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

--[[Unpacks a bitfield of ''n'' bits.
    Returns the least significant bit first.
  ]]
local function unpackflags(flags, n)
  local fl = {}
  local m,f = flags
  for i=1,n do
    m,f = modf(m/2)
    fl[#fl+1] = f>=0.5
  end
  return unpack(fl)
end

--[[Convert a string to a number.
    The bytes are read in LSB order.
  ]]
local function bytestonumber(b)
  local function _b2n(f, n, b, ...)
    if b then
      return _b2n(f*256, n + b*f, ...)
    end
    return n
  end
  return _b2n(256, strbyte(b,1,-1))
end

--[[Convert a number to a string with ''n'' bytes.
    The number is packed in LSB order.
  ]]
local function numbertobytes(n,f)
  local function _n2b(f, n, m)
    m = m * 256
    if f == 0 then
      return m
    end
    return m, _n2b(f-1, modf(n/256))
  end
  return strchar(_n2b(f-1, modf(n/256)))
end

--[[Read bytes from a file handle until 
    a NULL is read. Returns the string without
    the terminating NULL.
  ]]
local function readzstring(h)
  local s = {}
  local b = ""
  repeat
    s[#s+1] = b
    b = h:read(1)
  until b == '\0'
  return concat(s)
end

--[[Extracts the RFC1952 extra fields from a string.
    A table of [tag]=string pairs is returned. The 
    tag is the two subfield ID bytes concatenated.
  ]]
local function readextrafields(buf)
  local function _xtra(t, i)
    if i+4 > #buf then
      return t
    end
    local tag,len = sub(buf,1,2),sub(buf,3,4)
    len = bytestonumber(len)
    t[tag] = sub(buf,5,len+4)
    return _xtra(t, len+5)
  end
  return _xtra({}, 1)
end

--[[Method tables for gzfile.
  ]]
local gz_reader = {}
local gz_writer = {}

--[[Support the "*line" read argument.
  ]]
local function read_line(gz)
  if gz._eof and #gz._buffer == 0 then
    return nil
  end
  local handle, process = gz._handle, gz._process
  local buffer = {}
  local outbuf = gz._buffer
  local buflen = #outbuf
  local pos = find(outbuf, "\n", 1, true)
  while not pos and not gz._eof do
    buffer[#buffer+1] = outbuf
    local inbuf = handle:read(1024)
    if not inbuf then
      gz._eof = true
      break
    end
    local errmsg,errnum
    outbuf,errmsg,errnum = process(inbuf)
    assert(errnum>=0, errmsg, errnum)
    if errnum == zlib.Z_STREAM_END then
      -- FIXME should probably check the footer bytes or something
      gz._eof = true
    end
    buflen = buflen + #outbuf
    pos = find(outbuf, "\n", skip, true)
  end
  if buflen == 0 then
    return nil
  end
  if pos then
    gz._buffer = sub(outbuf, pos+1)
    gz._pos = gz._pos + buflen + pos - #outbuf
    buffer[#buffer+1] = sub(outbuf, 1, pos-1)
  else
    gz._buffer = ""
    gz._pos = gz._pos + buflen
    buffer[#buffer+1] = outbuf
  end
  return concat(buffer)
end

--[[Support the "*all" read argument.
  ]]
local function read_all(gz)
  if gz._eof and #gz._buffer == 0 then
    return ""
  end
  local handle, process = gz._handle, gz._process
  local buffer = { gz._buffer }
  local buflen = #buffer[1]
  gz._buffer = ""
  while not gz._eof do
    local inbuf = handle:read(1024)
    if not inbuf then
      gz._eof = true
      break
    end
    local outbuf,errmsg,errnum = process(inbuf)
    assert(errnum>=0, errmsg, errnum)
    if errnum == zlib.Z_STREAM_END then
      -- FIXME should probably check the footer bytes or something
      gz._eof = true
    end
    buflen = buflen + #outbuf
    buffer[#buffer+1] = outbuf
  end
  --if buflen == 0 then
  --  return nil
  --end
  gz._pos = gz._pos + buflen
  return concat(buffer)
end

--[[Read up to ''size'' bytes from a gzfile.
  ]]
local function read_bytes(gz, size)
  if gz._eof and #gz._buffer == 0 then
    return nil
  end
  if size <= 0 then
    return ""
  end
  local handle, process = gz._handle, gz._process
  local buffer = {}
  local outbuf = gz._buffer
  local buflen = #outbuf
  while buflen < size and not gz._eof do
    buffer[#buffer+1] = outbuf
    local inbuf = handle:read(1024)
    if not inbuf then
      gz._eof = true
      break
    end
    local errmsg,errnum
    outbuf,errmsg,errnum = process(inbuf)
    assert(errnum>=0, errmsg, errnum)
    if errnum == zlib.Z_STREAM_END then
      -- FIXME should probably check the footer bytes or something
      gz._eof = true
    end
    buflen = buflen + #outbuf
  end
  if buflen == 0 then
    return nil
  end
  -- pos is the offset from the end of the buffer
  local pos = size - buflen
  gz._buffer = sub(outbuf, #outbuf+pos+1)
  gz._pos = gz._pos + buflen + pos
  buffer[#buffer+1] = sub(outbuf, 1, #outbuf+pos)
  return concat(buffer)
end

--[[Standard file handle read method,
    but without "*number" conversion.
  ]]
function gz_reader:read(size)
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
local function read_skip(gz, size)
  if size <= 0 or (gz._eof and #gz._buffer == 0) then
    return gz._pos
  end
  local handle, process = gz._handle, gz._process
  local outbuf = gz._buffer
  local bytesread = #outbuf
  while bytesread < size and not gz._eof do
    local inbuf = handle:read(1024)
    if not inbuf then
      gz._eof = true
      break
    end
    local errmsg,errnum
    outbuf,errmsg,errnum = process(inbuf)
    assert(errnum>=0, errmsg, errnum)
    if errnum == zlib.Z_STREAM_END then
      -- FIXME should probably check the footer bytes or something
      gz._eof = true
    end
    bytesread = bytesread + #outbuf
  end
  -- pos is the offset from the end of the buffer
  local pos = size - bytesread
  gz._buffer = sub(outbuf, #outbuf+pos+1)
  gz._pos = gz._pos + bytesread + pos
  return gz._pos
end

--[[Rewind the file then skip to a new position.
    Fails if the underlying file handle doesn't 
    support seeking.
  ]]
local function read_rewind(gz, newpos)
  -- Return to the start of the compressed stream.
  assert(gz._zstreamstart and
      gz._handle:seek("set",gz._zstreamstart), "file handle cannot seek backwards")
  gz._eof = false
  gz._pos = 0
  gz._buffer = ""
  gz._process = zlib.decompressor{wbits=-15}
  return read_skip(gz, newpos)
end

--[[Standard file handle seek method
    for a gzfile in read mode.
    Seeking forward is possible by decompressing 
    and discarding bytes. To seek backward, the 
    stream must be completely rewound and read from the 
    beginning.
  ]]
function gz_reader:seek(whence, newpos)
  whence = whence or "cur"
  newpos = newpos or 0
  if whence == "end" then
    error("cannot seek from end of a gzfile")
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
    for a gzfile in read mode.
  ]]
function gz_reader:close()
  if self._ownhandle then
    self._handle:close()
    self._ownhandle = nil
  end
  self._handle = nil
  return true
end

--[[Convert values to strings and write to
    a gzfile.
  ]]
local function write_values(gz, val, ...)
  if not val then
    return gz
  end
  val = tostring(val)
  local outbuf,errmsg,errnum = gz._process(val)
  if errnum < 0 then
    return nil,errmsg,errnum
  end
  local res,message = gz._handle:write(outbuf)
  if not res then
    return nil,message
  end
  gz._crc32 = zlib.crc32(gz._crc32, val)
  gz._size = gz._size + #val
  return write_values(gz, ...)
end

--[[Standard write method.
  ]]
function gz_writer:write(...)
  assert(self._handle, "attempt to write to a closed file")
  return write_values(self, ...)
end

--[[Standard file handle seek.
    Can only seek forward while writing a gzfile.
  ]]
function gz_writer:seek(whence, newpos)
  whence = whence or "cur"
  newpos = newpos or 0
  if whence == "set" then
    newpos = newpos - self._size
  end
  assert(newpos>=0, "attempt to seek backwards while writing a gzfile")
  if newpos > 1024 then
    local str = strrep('\0', 1024)
    local crc = zlib.crc32(nil, str)
    while newpos >= 1024 do
      local outbuf,errmsg,errnum = self._process(str)
      if errnum < 0 then
        return nil,errmsg,errnum
      end
      local res,message = self._handle:write(outbuf)
      if not res then
        return nil,message
      end
      -- what a convenient function
      self._crc32 = zlib.crc32_combine(self._crc32, crc, 1024)
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
    self._crc32 = zlib.crc32(self._crc32, str)
    self._size = self._size + newpos
  end
  return self._size
end

--[[Standard close method
    for a gzfile in write mode.
  ]]
function gz_writer:close()
  if self._handle then
    local outbuf,errmsg,errnum = self._process(nil)
    if outbuf then
      self._handle:write(outbuf)
    end
    self._handle:write(numbertobytes(self._crc32,4), numbertobytes(self._size,4))
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
local function gz_mt_newindex()
  error("gzfile objects are read-only")
end

--[[gzfile metatables.
  ]]
local gz_read_mt = { __index=gz_reader, __newindex=gz_mt_newindex }
local gz_write_mt = { __index=gz_writer, __newindex=gz_mt_newindex }

--[[Open a gzfile in read mode.
    If ''ownhandle'' is set, then the file
    handle will be closed with the gzfile.
    A gzfile object will have the public fields:
      time - Time value saved in the gzip header.
      os - Name of the operating system that created the file.
      filename - Name of the original file (optional)
      comment - Description of the file (optional)
      extra - Table with subfield data (optional)
  ]]
local function gzfile_open(handle, ownhandle)
  local gz = { _handle=handle, _ownhandle=ownhandle }
  gz._process = zlib.decompressor{wbits=-15}
  local header,message = handle:read(10)
  if not header then
    return nil,message
  end
  if sub(header,1,3) ~= '\31\139\8' or #header < 10 then
    return nil, "Not a valid gzip file"
  end
  local fl,xf,os = strbyte(header, 4, 4), strbyte(header, 9, 10)
  local fa,fc,fx,fn,ft = unpackflags(fl, 5)
  if fx then -- extra field
    local len = bytestonumber(handle:read(2))
    gz.extra = readextrafields(handle:read(len))
  end
  if fn then -- file name
    gz.filename = readzstring(handle)
  end
  if ft then -- comment
    gz.comment = readzstring(handle)
  end
  if fc then -- header CRC
    handle:read(2)
  end
  gz.time = bytestonumber(sub(header,5,8))
  gz.os = os_code[os] or ("unknown ("..os..")")
  gz._buffer = ""
  gz._pos = 0
  gz._eof = false
  if handle.seek then -- Disregard if seeking isn't possible.
    gz._zstreamstart = handle:seek("cur",0)
  end
  return setmetatable(gz, gz_read_mt)
end

--[[Create a gzfile in write mode.
    If ''ownhandle'' is set, then the file
    handle will be closed with the gzfile.
  ]]
local function gzfile_create(handle, ownhandle, level, filename, comment)
  local gz = {
    _handle=handle,
    _ownhandle=ownhandle,
    _size=0,
    _crc32=zlib.crc32(),
    filename=filename,
    comment=comment
  }
  gz._process = zlib.compressor{level=level, wbits=-15}
  local fl = 0
  if filename then
    fl = fl + 8
  end
  if comment then
    fl = fl + 16
  end
  handle:write('\31\139\8', strchar(fl), '\0\0\0\0\0\255')
  if filename then
    handle:write(filename, '\0')
  end
  if comment then
    handle:write(comment, '\0')
  end
  return setmetatable(gz, gz_write_mt)
end

--[[Standard open function for gzfiles.
    The mode is either "r" or "w" and may also 
    have "b". (A gzfile is always in binary mode.)
    When writing a gzfile, the ''level'' option
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
    return gzfile_open(handle, ownhandle)
  else
    local name
    if type(file)=='string' then
      name = file
      if sub(name,-3) == '.gz' then
        name = sub(name, 1, -4)
      end
    end
    return gzfile_create(handle, ownhandle, level, name)
  end
end
