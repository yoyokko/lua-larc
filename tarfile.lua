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
local type,tonumber = type,tonumber
local setmetatable,getmetatable = setmetatable,getmetatable
local sub,rep = string.sub,string.rep
local find,match,gmatch = string.find,string.match,string.gmatch
local byte,char = string.byte,string.char
local format = string.format
local concat,insert = table.concat,table.insert
local modf = math.modf
local iopen = io.open
local rawset,pairs = rawset,pairs

local lfs = require "larc.filesystem"

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
local FTYPE_GNU_DIRECTORY = "D"
local FTYPE_GNU_LONGNAME = "L"
local FTYPE_GNU_LONGLINK = "K"
local FTYPE_GNU_MULTI = "M"
local FTYPE_GNU_NAMES = "N"
local FTYPE_GNU_SPARSE = "S"
local FTYPE_GNU_VOLUME = "V"
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
  [FTYPE_CONT] = "regular";
  [FTYPE_GNU_DIRECTORY] = "directory";
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
  return sub(s,1,n),rep("\0", n-#s)
end

local function unpackstring(s, i, n)
  i = i or 1
  n = n or -1
  local pos = find(s, "\0", i, true)
  if pos and pos - i < n then
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
  local b = byte(sub(s,i,i))
  if b == 0 then
    return nil -- empty field
  elseif b ~= 128 then
    -- space or NULL ... make up your *** mind
    return tonumber(match(s, "^ *([0-7]+)[ %z]", i) or 0, 8)
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
  return _n2s({"\128"}, f, modf(n/256))
end

local pax_copy_fields = {
  size='size',
  mtime='mtime',
  atime='atime',
  ctime='ctime',
  uid='uid',
  gid='gid',
  uname='user',
  gname='group',
  path='name',
  linkpath='linkname',
  ['SCHILY.devminor']='devmajor',
  ['SCHILY.devmajor']='devminor',
}
local pax_number_fields = {
  'size',
  'atime',
  'ctime',
  'mtime',
  'uid',
  'gid',
  'SCHILY.devminor',
  'SCHILY.devmajor',
  'SCHILY.dev',
  'SCHILY.ino',
  'SCHILY.nlinks',
}
local function add_pax(file)
  local pax = file.pax
  local function _add(p,f)
    if pax[p] then
      file[f or p] = pax[p]
    end
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

local function tarfile_fill_block(file, size)
  local block = assert(file._handle:read(512), "unexpected end of file")
  file._rem = file._rem - size
  file._bpos = size - #file._block
  file._block = block
  return sub(block, 1, size)
end

local function tarfile_use_block(file, size)
  local bpos,npos = file._bpos, file._bpos + size
  file._bpos = npos
  file._rem = file._rem - size
  return sub(file.block, bpos, npos-1)
end

local function tarfile_flush_block(file)
  local block = sub(file._block, file._bpos)
  local size = -file._bpos
  file._rem = file._rem + file._bpos
  file._block = ""
  file._bpos = 0
  return block,size
end

local function tarfile_read_buffer(file, buffer, size)
  buffer[#buffer+1] = assert(file._handle:read(size), "unexpected end of file")
  file._rem = file._rem - size
end

function tarfile:close()
  self._handle = nil
end
tarfile_sparse.close = tarfile.close

function tarfile:read(size)
  if self._pos == self.size then
    return nil
  end
  local opos = self._pos
  local npos = opos + size
  if npos > self.size then
    npos = self.size
    size = npos - opos
  end
  self._pos = npos
  self._handle:seek('set', self._fpos+opos)
  return self._handle:read(size)
end

function tarfile_sparse:read(size)
  if self._pos == self.size then
    return nil
  end
  if self._pos + size > self.size then
    size = self.size - self._pos
  end
  local buffer = {}
  while self._spos ~= 0 do
    if self._spos > 0 then
      if self._spos > size then
        buffer[#buffer+1] = rep("\0", size)
        self._spos = self._spos - size
        self._pos = self._pos + size
        break
      end
      buffer[#buffer+1] = rep("\0", self._spos)
      size = size - self._spos
      self._pos = self._pos + self._spos
      self._spos = -self._sparse.size
    else
      local sparse = self._sparse
      self._handle:seek('set', 
                   self._pos - sparse.start + sparse.position + self._fpos)
      if -self._spos > size then
        buffer[#buffer+1] = assert(self._handle:read(size), "unexpected end of file")
        self._spos = self._spos + size
        self._pos = self._pos + size
        break
      end
      buffer[#buffer+1] = assert(self._handle:read(-self._spos), "unexpected end of file")
      size = size + self._spos
      self._pos = self._pos - self._spos
      if sparse.next then
        sparse = sparse.next
        self._spos = sparse.zero
        self._sparse = sparse
      else
        self._spos = 0
      end
    end
  end
  return concat(buffer)
end

function tarfile:seek(whence, npos)
  npos = npos or 0
  local pos = self._pos
  if not whence or whence == 'set' then
    if npos == 0 then
      return pos
    end
  elseif whence == 'cur' then
    npos = pos + npos
  elseif whence == 'end' then
    npos = self.size + npos
  else
    error("invalid seek")
  end
  assert(npos>=0 and npos<=self.size, "invalid seek")
  if npos > pos then
    -- Seeking forward
    self._handle:seek('set', self._fpos + self._pos)
    local size = npos - pos
    while size > 10240 do
      self._handle:read(10240) -- seek may not be supported
      size = size - 10240
    end
    self._handle:read(size)
  elseif npos < pos then
    -- Seeking backward, file handle needs to support this
    self._handle:seek('set', self._fpos + npos)
  end
  self._pos = npos
  return npos
end

function tarfile_sparse:seek(whence, npos)
  npos = npos or 0
  local pos = self._pos
  if not whence or whence == 'set' then
    if npos == 0 then
      return pos
    end
  elseif whence == 'cur' then
    npos = pos + npos
  elseif whence == 'end' then
    npos = self.size + npos
  else
    error("invalid seek")
  end
  assert(npos>=0 and npos<=self.size, "invalid seek")
  local sparse = self._sparse
  if npos > pos then
    -- Seeking forward
    self._handle:seek('set', 
                 self._pos - sparse.start + sparse.position + self._fpos)
    local size = npos - pos
    while size > 0 do
      if self._spos > 0 then
        local spos = self._spos - size
        size = size - self._spos
        if spos <= 0 then
          spos = -sparse.size
        end
        self._spos = spos
      else
        local spos = self._spos + size
        if spos < 0 then
          while size > 10240 do
            self._handle:read(10240) -- seek may not be supported
            size = size - 10240
          end
          self._handle:read(size)
          size = 0
          self._spos = spos
        else
          local ssize = -self._spos
          while ssize > 10240 do
            self._handle:read(10240) -- seek may not be supported
            ssize = ssize - 10240
          end
          self._handle:read(ssize)
          size = size + self._spos
          if sparse.next then
            sparse = sparse.next
            self._spos = sparse.zero
            self._sparse = sparse
          else
            self._spos = 0
            assert(size==0, "unexpected end of file")
          end
        end
      end
    end
  elseif npos < pos then
    -- Seeking backward, file handle needs to support this
    while sparse.start-sparse.zero > npos do
      sparse = assert(sparse.prev, "internal error")
    end
    self._sparse = sparse
    if npos < sparse.start then
      self._spos = sparse.start - npos
      assert(self._spos<=sparse.zero, "internal error")
      self._handle:seek('set', self._fpos + sparse.position)
    else
      local spos = sparse.size - sparse.start - npos
      assert(spos>0, "internal error")
      self._spos = -spos
      self._handle:seek('set', 
                   npos - sparse.start + sparse.position + self._fpos)
    end
  end
  self._pos = npos
  return npos
end

local tarfile_mt = {
  __index=tarfile,
  __newindex=tar_mt_newindex
}
local tarfile_sparse_mt = {
  __index=tarfile_sparse,
  __newindex=tar_mt_newindex
}

local function tarfile_open(tar, file, size, sparse)
  file._handle = tar._handle
  file._fpos = tar._handle:seek()
--file._size = file.size
  file._pos = 0
--[[
  file._rem = file.size -- The number of unread bytes in the file
  file._bpos = 0 -- Negative offset from the end of the last read block
  file._block = ""
]]
  if sparse then
    file._sparse = sparse
    if sparse.start == 0 then
      file._spos = -sparse.size
    else
      file._spos = sparse.start
    end
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
      rawset(tar, '_eof', true)
      return nil, "read error"
    end
  until match(block, "[^%z]") -- skip NULL blocks
  local magic = sub(block, 258, 265)
  if magic~=POSIX_FORMAT and magic~=GNU_FORMAT then
    return nil, "unsupported format"
  end
  local checksum = unpacknumber(block, 149, 8)
  if not checksum then
    return nil, "checksum missing"
  end
  local sum,ssum = calculate_checksum(block)
  if checksum~=sum and checksum~=ssum then
    return nil, "checksum mismatch"
  end
  file = file or {headersize=0}
  file.headersize = file.headersize + #block
  file.position = tar._handle:seek()
  file.size = unpacknumber(block, 125, 12)
  if not file.size then
    return nil, "malformed header"
  end
  file.type = sub(block, 157, 157)
  if file.type == FTYPE_REG0 then
    file.type = FTYPE_REG
  end
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
  local magic = sub(block, 258, 265)
  if not file.name then -- Name may already been set by a previous header
    file.name = unpackstring(block, 1, 100)
    if magic == POSIX_FORMAT then
      local prefix = unpackstring(block, 346, 155)
      if prefix ~= "" then
        file.name = prefix .. "/" .. file.name
      end
    end
  end
  file.mode = unpacknumber(block, 101, 8)
  file.uid = unpacknumber(block, 109, 8)
  file.user = unpackstring(block, 266, 32)
  file.gid = unpacknumber(block, 117, 8)
  file.group = unpackstring(block, 298, 32)
  file.mtime = unpacknumber(block, 137, 12)
  if magic == GNU_FORMAT then
    file.atime = unpacknumber(block, 346, 12)
    file.ctime = unpacknumber(block, 358, 12)
  end
  if file.type == FTYPE_REG and sub(file.name, -1)=="/" then
    file.type = FTYPE_DIR
  end
  if file.pax then
    add_pax(file)
  end
  return file
end

local function tar_readheader_device(tar, file, block)
  file.devmajor = unpacknumber(block, 330, 8)
  file.devminor = unpacknumber(block, 338, 8)
  file.size = 0
  return tar_readheader_file(tar, file, block)
end

local function tar_readheader_special(tar, file, block)
  file.size = 0
  return tar_readheader_file(tar, file, block)
end

local function tar_readheader_link(tar, file, block)
  if not file.linkname then
    file.linkname = unpackstring(block, 158, 100)
  end
  file.size = 0
  return tar_readheader_file(tar, file, block)
end

local function tar_readheader_sparse(tar, file, block)
  local pos = 0
  local next = 0
  local chunkhead = {}
  local chunk = chunkhead
  for i = 387,459,24 do
    local o,s = unpacknumber(block, i, 12), unpacknumber(block, i+12, 12)
    if not o then
      break
    end
    if s ~= 0 then
      chunk.next = { start=o, size=s, zero=o-next, position=pos, prev=chunk }
      chunk = chunk.next
      pos = pos + s
      next = o + s
    end
  end
  local extended = byte(block, 483)
  while extended ~= 0 do
    local eblock = tar._handle:read(512)
    file.headersize = file.headersize + #eblock
    for i = 1,457,24 do
      local o,s = unpacknumber(eblock, i, 12), unpacknumber(eblock, i+12, 12)
      if not o then
        break
      end
      if s ~= 0 then
        chunk.next = { start=o, size=s, zero=o-next, position=pos, prev=chunk }
        chunk = chunk.next
        pos = pos + s
        next = o + s
      end
    end
    extended = byte(eblock, 505)
  end
  file.real_size = unpacknumber(block, 484, 12)
  if next < file.real_size then
    -- This creates an empty chunk for files that have trailing NULL bytes
    chunk.next = { start=file.real_size, size=0, zero=file.real_size-next, position=pos }
  end
  chunkhead.next.prev = nil
  file.sparse_struct = chunkhead.next
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

local function tar_readheader_longdir(tar, file, block)
  error("GNU directory entries are not supported")
end

local function tar_readheader_multipart(tar, file, block)
  error("GNU multi-volume archives are not supported")
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
tar_readheader_types[FTYPE_PIPE] = tar_readheader_special
tar_readheader_types[FTYPE_CONT] = tar_readheader_special
tar_readheader_types[FTYPE_DIR] = tar_readheader_file
tar_readheader_types[FTYPE_GNU_DIRECTORY] = tar_readheader_longdir
tar_readheader_types[FTYPE_GNU_LONGLINK] = tar_readheader_longlink
tar_readheader_types[FTYPE_GNU_LONGNAME] = tar_readheader_longname
tar_readheader_types[FTYPE_GNU_MULTI] = tar_readheader_multipart
tar_readheader_types[FTYPE_GNU_NAMES] = tar_readheader_file
tar_readheader_types[FTYPE_GNU_SPARSE] = tar_readheader_sparse
tar_readheader_types[FTYPE_GNU_VOLUME] = tar_readheader_file
tar_readheader_types[FTYPE_POSIX_GLOBAL] = tar_readheader_global
tar_readheader_types[FTYPE_POSIX_EXT] = tar_readheader_pax
tar_readheader_types[FTYPE_SOLARIS_EXT] = tar_readheader_pax
setmetatable(tar_readheader_types, {
  __index = function (t, k) return tar_readheader_file end
  })


local function tar_extractfile(tar, file, dest, progress, progbytes)
  local name = file.name
  local function output(inf, outf, size)
    local size = file.size
    while size > 0 do
      local bufsize
      if size > 10240 then
        bufsize = 10240
      else
        bufsize = size
      end
      local buf = inf:read(bufsize)
      if not buf then
        return false,"unexpected end of file"
      end
      out:write(buf)
      size = size - bufsize
      progbytes = progbytes + bufsize
      progress(name, progbytes)
    end
  end
  local fullname = lfs.joinpath(dest, lfs.normalize(name))
  progress(name, progbytes)
  local t = file.type
  if t == FTYPE_DIR then
    return lfs.makedirs(fullname)
  end
  while t == FTYPE_LINK do
    -- Does not preserve hard links.
    local link = tar._names[file.linkname]
    if not link then
      t = FTYPE_SYMLINK
    else
      file = link
      t = file.type
    end
  end
  lfs.makedirstofile(fullname)
  if t == FTYPE_SYMLINK then
    return lfs.makesymlink(fullname, file.linkname)
  elseif t == FTYPE_PIPE then
    return lfs.makepipe(fullname)
  elseif t == FTYPE_BDEV then
    return lfs.makedevice(fullname, "b", file.devmajor, file.devminor)
  elseif t == FTYPE_CDEV then
    return lfs.makedevice(fullname, "c", file.devmajor, file.devminor)
  else -- file.type == FTYPE_REG i hope
    local out,message = iopen(fullname, "wb")
    if not out then
      return false, message
    end
    if file.sparse_struct then
      local block = file.sparse_struct
      out:seek('set', file.real_size-1)
      out:write("\0")
      tar._handle:seek('set', file.position)
      while block do
        out:seek('set', block.start)
        local success,message = output(tar._handle, out, block.size)
        if not success then
          out:close()
          return false,message
        end
        block = block.next
      end
    else
      if file.size ~= 0 then
        tar._handle:seek('set', file.position)
        local success,message = output(tar._handle, out, file.size)
        if not success then
          out:close()
          return false,message
        end
      end
    end
    out:close()
  end
  return true
end

local function tar_nextfile(tar, file)
  local pos, lastfile
  if not file then
    file = tar._firstfile
    pos = tar._startpos
  else
    pos = align(file.position + file.size)
    lastfile = file
    file = file.next
  end
  if not file then
    tar._handle:seek('set', pos)
    local message
    file,message = tar_readheader(tar)
    if message then
      return nil,message
    end
    if file then
      if lastfile then
        lastfile.next = file
      end
      tar._lastfile = file
      tar._names[file.name] = file
    end
  end
  return file
end

local function tar_getfile(tar, name)
  assert(type(name)=='string', "invalid argument")
  local file = tar._names[name]
  if not file and not tar._eof then
    file = tar_nextfile(tar, tar._lastfile)
    while file do
      if file.name == name then
        return file
      end
      file = tar_nextfile(tar, file)
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
    devmajor = file.devmajor,
    devminor = file.devminor
  }
  if file.pax then
    copy_pax(info, file.pax)
  end
  return info
end

local tar = {}

local function dummy_progress() end

--[[Decompress and save a file.
    With dest, saves to the given directory using the file 
    name from the archive, including sub-directories.
    Without dest, saves to the current directory using the
    file name from the archive, including sub-directories.
    Without name or dest, extracts all files to the current
    directory.
  ]]
function tar:extract(name, dest, progress)
  dest = dest or ""
  progress = progress or dummy_progress
  if name then
    local file,message = tar_getfile(self, name)
    if message then
      return false,message
    end
    return tar_extractfile(self, file, dest, progress, 0)
  else
    local file,message = tar_nextfile(self)
    if message then
      return false,message
    end
    local prev = 0
    while file do
      local success,message = tar_extractfile(self, file, dest, progress, prev)
      if not success then
        return false,message
      end
      if file.type ~= FTYPE_DIR and file.type ~= FTYPE_GNU_DIRECTORY then
        prev = prev + (file.real_size or file.size)
      end
      file,message = tar_nextfile(self, file)
      if message then
        return false,message
      end
    end
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
  local file = assert(tar_getfile(self, name), "file not found")
  if file.size == 0 or file.type == FTYPE_DIR then
    return ""
  end
  if not size or size < file.size then
    size = file.size
  end
  self._handle:seek('set', file.position)
  return self._handle:read(size)
end

function tar:open(name)
  local file, message = tar_getfile(self, name)
  if not file then
    return nil, message
  end
  self._handle:seek('set', file.position)
  return tarfile_open(self, tar_fileinfo(file), file.size, file.sparse_struct)
end

function tar:names()
  local file
  return function(self)
      local message
      file,message = tar_nextfile(self, file)
      if message then
        error(message)
      end
      if not file then
        return nil
      end
      return file.name
    end, self
  --[[
  local file = self._firstfile
  local pos = 0
  return function(self)
      if not file then
        self._handle:seek('set', pos)
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
  ]]
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
  tar._startpos = handle:seek()
  tar._names = {}
  local file, message = tar_nextfile(tar)
  if message then
    return nil, message
  end
  return setmetatable(tar, tar_mt)
end


function open(file, mode)
  mode = mode or "r"
  do
    local tn = type(file)
    assert(tn=='string' or ((
        tn=='table' or tn=='userdata') and type(file.read)=='function'),
          "invalid file handle")
    assert(type(mode)=='string')
  end
  if mode ~= "r" then
    error("invalid mode '"..mode.."' (tar files can only be opened for reading)")
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
