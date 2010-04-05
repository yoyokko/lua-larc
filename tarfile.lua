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

local assert,error = assert,error
local type,setmetatable,tonumber = type,setmetatable,tonumber
local sub,rep = string.sub,string.rep
local find,match,gmatch = string.find,string.match,string.gmatch
local byte,char = string.byte,string.char
local format = string.format
local concat,insert = table.concat,table.insert
local modf = math.modf
local iopen = io.open
local rawset,pairs = rawset,pairs

module "larc.tarfile"

local POSIX_FORMAT = "ustar\00000"
local GNU_FORMAT = "ustar  \0"
local FTYPE_REG0 = "\0"
local FTYPE_REG = "0"
local FTYPE_LINK = "1"
local FTYPE_SYMLINK = "2"
local FTYPE_CDEV = "3"
local FTYPE_BDEV = "4"
local FTYPE_DIR = "5"
local FTYPE_PIPE = "6"
local FTYPE_CONT = "7"
local FTYPE_GNU_LONGNAME = "L"
local FTYPE_GNU_LONGLINK = "K"
local FTYPE_GNU_SPARSE = "S"
local FTYPE_POSIX_GLOBAL = "g"
local FTYPE_POSIX_EXT = "x"
local FTYPE_SOLARIS_EXT = "X"

local tar_type_name = {
  [FTYPE_REG0] = "regular";
  [FTYPE_REG] = "regular";
  [FTYPE_LINK] = "link";
  [FTYPE_SYMLINK] = "symlink";
  [FTYPE_CDEV] = "character device";
  [FTYPE_BDEV] = "block device";
  [FTYPE_DIR] = "directory";
  [FTYPE_PIPE] = "fifo";
  [FTYPE_CONT] = "regular"; -- I have no idea what this means
  [FTYPE_GNU_LONGNAME] = "Name Extension";
  [FTYPE_GNU_LONGLINK] = "Link Extension";
  [FTYPE_GNU_SPARSE] = "regular";
  [FTYPE_POSIX_GLOBAL] = "PAX Header";
  [FTYPE_POSIX_EXT] = "pax header";
  [FTYPE_SOLARIS_EXT] = "Extended Header";
}
setmetatable(tar_type_name, {
    __index = function(t, k) return "unknown type \""..k.."\"" end
  })

local function align(n)
  return modf((n + 511)/512) * 512
end

local function padstring(s, n)
  return sub(s,1,n),rep('\0', n-#s)
end

local function unpackstring(s, i, n)
  i = i or 1
  n = n or -1
  local pos = find(s, '\0', i, true)
  if pos - i < n then
    return sub(s, i, pos-1)
  end
  return sub(s, i, i+n-1)
end

local function unpacknumber(s, i, n)
  local function _s2n(n, b, ...)
    if not b then
      return n
    end
    return _s2n(n*256 + b, ...)
  end
  if char(sub(s,i,i)) ~= 128 then
    -- libarchive puts spaces where a NULL should be
    return tonumber(match(s, " *([0-7]+)[ %z]", i) or 0, 8)
  else
    return _s2n(0, byte(s, i+1, i+n-1))
  end
end

local function packnumber(n, f)
  f = f - 1
  assert(n>=0 and n<8^f, "number too large to pack")
  return format("%0"..f.."o\0", n)
end
local function packnumber_gnu(n, f)
  local function _n2s(t, f, n, m)
    if f == 0 then
      return concat(t)
    end
    insert(t, 2, char(m * 256))
    return _n2s(t, f-1, modf(n/256))
  end
  f = f - 1
  if n >= 0 and n < 8^f then
    return packnumber(n, f+1)
  end
  -- TODO: negative numbers
  return _n2s({'\128'}, f, modf(n/256))
end

local pax_copy_fields = {
  size='size',
  mtime='mtime',
  uid='uid',
  gid='gid',
  uname='user',
  gname='group',
  path='name',
  linkpath='linkname'
}
local pax_number_fields = {
  'size',
  'atime',
  'ctime',
  'mtime',
  'uid',
  'gid',
}
local function add_pax(file)
  local pax = file.pax
  local function _add(p,f)
    if pax[p] then
      file[f or p] = pax[p]
    end
  end
  local function _num(f)
    pax[f] = tonumber(pax[f])
  end
  if not pax then
    return
  end
  for _,f in pairs(pax_number_fields) do
    if pax[f] then
      pax[f] = tonumber(pax[f])
    end
  end
  for p,f in pairs(pax_copy_fields) do
    if pax[p] then
      file[f] = pax[p]
    end
  end
end
local function copy_pax(file, pax)
  for k,v in pairs(pax) do
    if not pax_copy_fields[k] then
      local t=file
      local n = k
      local p,f = match(n, "^(%w-)%.(.+)$")
      while p do
        t[p] = t[p] or {}
        t = t[p]
        n = f
        p,f = match(n, "^(%w-)%.(.+)$")
      end
      t[n] = v
    end
  end
end

local function calculate_checksum(block)
  local function tosigned(b)
    if b >= 128 then
      return b - 256
    end
    return b
  end
  local sum = 256
  local ssum = 256
  for i = 1,148,4 do
    local a,b,c,d = byte(block, i, i+3)
    sum = sum + a + b + c + d
    ssum = ssum + tosigned(a) + tosigned(b) + tosigned(c) tosigned(d)
  end
  for i = 157,512,4 do
    local a,b,c,d = byte(block, i, i+3)
    sum = sum + a + b + c + d
    ssum = ssum + tosigned(a) + tosigned(b) + tosigned(c) tosigned(d)
  end
  return sum, ssum
end

local function tar_mt_newindex()
  error("tarfile objects are read-only")
end

local tarfile = {}
local tarfile_sparse = {}

function tarfile:close()
  self._handle = nil
end
tarfile_sparse.close = tarfile.close

function tarfile:read(size)
  if self._rem == 0 then
    return nil
  end
  if size > self._rem then
    size = self._rem
  end
  if size <= -self._bpos then
    local bpos,npos = self._bpos, self._bpos + size
    self._bpos = npos
    return sub(self._block, bpos, npos-1)
  end
  local buffer = {sub(self._block, self._bpos)}
  self._rem = self._rem + self._bpos
  size = size + self._bpos
  self._block = ""
  self._bpos = 0
  while size > 4096 do
    buffer[#buffer+1] = assert(self._handle:read(4096), "unexpected end-of-file")
    self._rem = self._rem - 4096
    size = size - 4096
  end
  while size > 512 do
    buffer[#buffer+1] = assert(self._handle:read(512), "unexpected end-of-file")
    self._rem = self._rem - 512
    size = size - 512
  end
  self._block = assert(self._handle:read(512), "unexpected end-of-file")
  self._rem = self._rem - size
  self._bpos = size - #self._block
  buffer[#buffer+1] = sub(self._block, 1, size)
  return concat(buffer)
end

function tarfile_sparse:read(size)
end

function tarfile:seek(whence, npos)
  npos = npos or 0
  local pos = self.size - self._rem
  if not whence or whence == "set" then
    if npos == 0 then
      return pos
    end
  elseif whence == "cur" then
    npos = pos + npos
  elseif whence == "end" then
    npos = self.size + npos
  else
    error("invalid seek")
  end
  assert(npos>=0 and npos<=self.size, "invalid seek")
  -- Reset current position to the start of the last read block
  pos = #self._block + self._bpos + self._rem
  if (self.size - npos) <= pos then
    -- Seeking forward
    local size = pos - (self.size - npos)
    self._rem = pos
    while size > 512 do
      self._handle:read(512) -- seek may not be supported
      self._rem = self._rem - 512
      size = size - 512
    end
    self._block = assert(self._handle:read(512), "unexpected end-of-file")
    self._bpos = size - #self._block
    self._rem = self._rem - size
  else
    -- Seeking backward, file handle needs to support this
    local pos,rem = modf(npos/512)
    pos,rem = pos*512, rem*512
    self._handle:seek("set", self._fpos + pos)
    self._block = assert(self._handle:read(512), "unexpected end-of-file")
    self._bpos = rem - #self._block
    self._rem = self.size - npos
  end
  return npos
end

function tarfile_sparse:seek(whence, npos)
end

local tarfile_mt = { __index=tarfile, __newindex=tar_mt_newindex }
local tarfile_sparse_mt = { __index=tarfile_sparse, __newindex=tar_mt_newindex }

local function tarfile_open(tar, file, size, sparse)
  file._handle = tar._handle
  file._fpos = tar._handle:seek()
  file._size = size
  file._rem = file.size -- The number of unread bytes in the file
  file._bpos = 0 -- Negative offset from the end of the last read block
  file._block = ""
  if sparse then
    file._sparse = sparse
    return setmetatable(file, tarfile_sparse_mt)
  end
  return setmetatable(file, tarfile_mt)
end


local tar_readheader_types = {}

local function tar_readheader(tar, file)
  local block
  repeat
    block = tar._handle:read(512)
    if not block then
      rawset(tar, "_eof", true)
      return nil
    end
  until match(block, "[^%z]") -- skip NULL blocks
  local magic = sub(block, 258, 265)
  assert(magic==POSIX_FORMAT or magic==GNU_FORMAT, "unsupported tar format")
  local checksum = unpacknumber(block, 149, 8)
  local sum,ssum = calculate_checksum(block)
  if checksum~=sum and checksum~=ssum then
    return nil, "checksum mismatch"
  end
  file = file or {headersize=0}
  file.headersize = file.headersize + #block
  file.position = tar._handle:seek()
  file.size = unpacknumber(block, 125, 12)
  file.type = sub(block, 157, 157)
  if tar._pax then
    local pax = {}
    for k,v in pairs(tar._pax) do
      pax[k] = v
    end
    file.pax = pax
  end
  return tar_readheader_types[file.type](tar, file, block)
end

local function tar_readheader_file(tar, file, block)
  if not file.name then -- Name may already been set by a previous header
    file.name = unpackstring(block, 1, 100)
    local prefix = unpackstring(block, 346, 155)
    if prefix ~= "" then
      file.name = prefix .. "/" .. file.name
    end
  end
  file.mode = unpacknumber(block, 101, 8)
  file.uid = unpacknumber(block, 109, 8)
  file.user = unpackstring(block, 266, 32)
  file.gid = unpacknumber(block, 117, 8)
  file.group = unpackstring(block, 298, 32)
  file.mtime = unpacknumber(block, 137, 12)
  if file.type == FTYPE_REG0 and sub(file.name, -1)=='/' then
    file.type = FTYPE_DIR
  end
  if file.pax then
    add_pax(file)
  end
  return file
end

local function tar_readheader_device(tar, file, block)
  file.major = unpacknumber(block, 330, 8)
  file.minor = unpacknumber(block, 338, 8)
  return tar_readheader_file(tar, file, block)
end

local function tar_readheader_link(tar, file, block)
  if not file.linkname then
    file.linkname = unpacknumber(block, 158, 100)
  end
  return tar_readheader_file(tar, file, block)
end

local function tar_readheader_sparse(tar, file, block)
  local chunk = {}
  local pos = 0
  for i = 387,459,24 do
    local o,s = unpacknumber(block, i, 12), unpacknumber(block, i+12, 12)
    chunk.start = o
    chunk.size = s
    chunk.position = pos
    chunk.prev = file.sparse_struct
    file.sparse_struct = chunk
    pos = pos + size
  end
  local extended = byte(block, 483)
  while extended ~= 0 do
    local eblock = tar._handle:read(512)
    file.headersize = file.headersize + #eblock
    for i = 1,457,24 do
      local o,s = unpacknumber(eblock, i, 12), unpacknumber(eblock, i+12, 12)
      chunk.start = o
      chunk.size = s
      chunk.position = pos
      chunk.prev = file.sparse_struct
      file.sparse_struct = chunk
      pos = pos + size
    end
    extended = byte(eblock, 505)
  end
  file.real_size = unpacknumber(block, 484, 12)
  file.position = tar._handle:seek()
  return tar_readheader_file(tar, file, block)
end

local function tar_readheader_longlink(tar, file, block)
  block = tar._handle:read(align(file.size))
  file.linkname = unpackstring(block)
  file.headersize = file.headersize + #block
  return tar_readheader(tar, file)
end

local function tar_readheader_longname(tar, file, block)
  block = tar._handle:read(align(file.size))
  file.name = unpackstring(block)
  file.headersize = file.headersize + #block
  return tar_readheader(tar, file)
end

local function tar_readheader_pax(tar, file, block)
  block = tar._handle:read(align(file.size))
  local pax = file.pax or {}
  for n,k,v in gmatch(block, "(%d+) ([^=]+)=([^\10]+)\10") do
    -- You could check that the given length is the same as what is matched
    -- ... but I don't because I'm lazy
    pax[k] = v
  end
  file.pax = pax
  return tar_readheader(tar, file)
end

local function tar_readheader_global(tar, file, block)
  block = tar._handle:read(align(file.size))
  local pax = tar._pax or {}
  for n,k,v in gmatch(block, "(%d+) ([^=]+)=([^\10]+)\10") do
    -- You could check that the given length is the same as what is matched
    -- ... but I don't because I'm lazy
    pax[k] = v
  end
  tar._pax = pax
  return tar_readheader(tar, file)
end

tar_readheader_types[FTYPE_REG0] = tar_readheader_file
tar_readheader_types[FTYPE_REG] = tar_readheader_file
tar_readheader_types[FTYPE_LINK] = tar_readheader_link
tar_readheader_types[FTYPE_SYMLINK] = tar_readheader_link
tar_readheader_types[FTYPE_CDEV] = tar_readheader_device
tar_readheader_types[FTYPE_BDEV] = tar_readheader_device
tar_readheader_types[FTYPE_PIPE] = tar_readheader_file
tar_readheader_types[FTYPE_CONT] = tar_readheader_file
tar_readheader_types[FTYPE_DIR] = tar_readheader_file
tar_readheader_types[FTYPE_GNU_LONGLINK] = tar_readheader_longlink
tar_readheader_types[FTYPE_GNU_LONGNAME] = tar_readheader_longname
tar_readheader_types[FTYPE_GNU_SPARSE] = tar_readheader_sparse
tar_readheader_types[FTYPE_POSIX_GLOBAL] = tar_readheader_global
tar_readheader_types[FTYPE_POSIX_EXT] = tar_readheader_pax
tar_readheader_types[FTYPE_SOLARIS_EXT] = tar_readheader_pax
setmetatable(tar_readheader_types, {
  __index = function (t, k) return tar_readheader_file end
  })


local function tar_getfile(tar, name)
  assert(type(name)=='string')
  local file,message = tar._names[name]
  if not file and not tar._eof then
    file = tar._lastfile
    while file do
      if file.name == name then
        break
      end
      local pos = align(file.position + file.size)
      tar._handle:seek("set", pos)
      file,message = tar_readheader(tar)
      if message then
        return nil, message
      end
    end
  end
  if not file then
    return nil, name.." not in archive"
  end
  return file
end

local function tar_fileinfo(file)
  local info = {
    name = file.name,
    mode = file.mode,
    uid = file.uid,
    user = file.user,
    gid = file.gid,
    group = file.group,
    size = file.real_size or file.size,
    time = file.mtime,
    type = tar_type_name[file.type],
    linkname = file.linkname,
    major = file.major,
    minor = file.minor
  }
  if file.pax then
    copy_pax(info, file.pax)
  end
  return info
end

local tar = {}

--[[Decompress and save a file.
    Without dest, saves to the current directory using the
    file name from the archive, including sub-directories.
    Without name or dest, extracts all files to the current
    directory.
  ]]
function tar:extract(name, dest)
  if dest then
    local t = strsub(dest, -1)
    if t ~= '/' or t ~= path_sep then
      dest = dest .. path_sep
    end
  else
    dest = ""
  end
  if name then
  else
  end
  return true
end

function tar:getinfo(name)
  local file, message = tar_getfile(self, name)
  if not file then
    return nil, message
  end
  return tar_fileinfo(file)
end

function tar:read(name, size)
  local file, message = assert(tar_getfile(self, name))
  if file.size == 0 then
    return ""
  end
  if not size or size < file.size then
    size = file.size
  end
  self._handle:seek("set", file.position)
  return self._handle:read(size)
end

function tar:open(name)
  local file, message = tar_getfile(self, name)
  if not file then
    return nil, message
  end
  self._handle:seek("set", file.position)
  return tarfile_open(self, tar_fileinfo(file), file.size, file.sparse_struct)
end

function tar:names()
  local file = self._firstfile
  local pos = 0
  return function(self)
      if not file then
        self._handle:seek("set", pos)
        file,message = tar_readheader(self)
        if message then
          error(message)
        end
        if not file then
          return nil
        end
      end
      local name = file.name
      pos = align(file.position + file.size)
      file = file.next
      return name
    end, self
end

function tar:close()
  if self._ownhandle then
    self._handle:close()
    self._ownhandle = nil
  end
end

local tar_mt = { __index=tar, __newindex=tar_mt_newindex }

local function tar_open(handle, ownhandle)
  local tar = { _handle = handle, _ownhandle = ownhandle }
  tar._names = {}
  local file, message = tar_readheader(tar)
  if message then
    return nil, message
  end
  tar._firstfile = file
  tar._lastfile = file
  return setmetatable(tar, tar_mt)
end


function open(file, mode)
  mode = mode or "r"
  do
    local tn = type(file)
    assert(tn=='string' or ((
        tn=='table' or tn=='userdata') and type(file.read)=='function'))
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
  tar,message = tar_open(handle, ownhandle)
  if not tar then
    return nil, "Not a valid tar archive: "..message
  end
  return tar
end

return _M
