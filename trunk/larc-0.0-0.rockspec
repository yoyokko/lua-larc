package = "LArc"
version = "0.0-0"
source = {
  url = ""
}
description = {
  summary = "Library for reading compressed files in a variety of formats.",
  detailed = [[The LArc library can read multi-file
    archives like zip and tar as well as compressed
    streams like gzip and bzip2.
  ]],
  homepage = "http://code.google.com/p/lua-larc",
  license = "BSD"
}
dependencies = {
  "lua >= 5.1",
  "luafilesystem >= 1.5"
}
external_dependencies = {
  ZLIB = { header="zlib.h" },
  BZ2 = { header="bzlib.h" },
  LZMA = { header="lzma.h" }
}
build = {
  type = "builtin",
  modules = {
    ["larc.zlib"] = {
      sources = { "lzlib.c" },
      libraries = { "z" },
      incdirs = { "$(ZLIB_INCDIR)" },
      libdirs = { "$(ZLIB_LIBDIR)" }
    },
    ["larc.bzip2"] = {
      sources = { "lbzip2.c" },
      libraries = { "bz2" },
      incdirs = { "$(BZ2_INCDIR)" },
      libdirs = { "$(BZ2_LIBDIR)" }
    },
    ["larc.lzma"] = {
      sources = { "llzma.c" },
      libraries = { "lzma" },
      incdirs = { "$(LZMA_INCDIR)" },
      libdirs = { "$(LZMA_LIBDIR)" }
    },
    ["larc.struct"] = "struct.c",
    ["larc.bz2file"] = "bz2file.lua",
    ["larc.gzfile"] = "gzfile.lua",
    ["larc.lzmafile"] = "lzmafile.lua",
    ["larc.tarfile"] = "tarfile.lua",
    ["larc.zipfile"] = "zipfile.lua",
    ["larc.ziploader"] = "ziploader.lua",
  }
}
