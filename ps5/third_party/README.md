# Third-party code vendored for the PS5 port

The PS5 payload SDK has no zlib or zstd, and PCSX2 takes both from its dependency build rather than
`3rdparty/`. The port needs them for CHD images (vk-285-108): `3rdparty/libchdr` decompresses hunks with
zlib (the `zlib` and `cdzl` codecs), zstd (`zstd`, `cdzs`), LZMA (`lzma`, `cdlz`; from `3rdparty/lzma`),
Huffman and FLAC (in libchdr). Only the decompression sources are here, byte-identical to the releases:

| Folder | Release | Tarball SHA-256 | License |
|---|---|---|---|
| `zlib/` | zlib 1.3.1 (`zlib-1.3.1.tar.gz`, zlib.net) | `9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23` | zlib (`zlib/LICENSE`) |
| `zstd/` | zstd 1.5.6 (`zstd-1.5.6.tar.gz`, github.com/facebook/zstd releases) | `8c29e06cf42aacc1eafc4077ae2ec6c6fcb96a626157e0593d5e82a34fd403c1` | BSD (`zstd/LICENSE`), used under BSD; also GPLv2 (`zstd/COPYING`) |

- **zlib:** inflate only: `adler32.c crc32.c inffast.c inflate.c inftrees.c zutil.c` and their headers
  (`gzguts.h` because `zutil.c` includes it). No deflate, no gz* file functions.
- **zstd:** `lib/zstd.h`, `lib/zstd_errors.h`, `lib/common/` without `pool.*` and `threading.*`, and
  `lib/decompress/` without `huf_decompress_amd64.S` (built with `ZSTD_DISABLE_ASM`). No compression,
  dictionary builder or legacy formats.

`ps5/coreorbis/Makefile.vk` builds them (`CHD_CSRCS`).
