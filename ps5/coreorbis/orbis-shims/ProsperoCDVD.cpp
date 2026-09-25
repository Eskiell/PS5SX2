// Orbis CDVD stubs: compressed image formats (.cso/.zso/.chd/.gz) need
// zlib/libchdr, absent from ps5-payload-sdk. Raw .iso/.bin works fully via
// the real InputIsoFile (kept in build); only compressed backends refuse.
// Physical-disc reader (Linux ioctl) likewise unavailable.
#include "CDVD/CsoFileReader.h"
#include "CDVD/ChdFileReader.h"
#include "CDVD/GzippedFileReader.h"
#include "CDVD/CDVDdiscReader.h"
#include "common/Error.h"

// (disc globals live in CDVDdiscReader.cpp already.)

IOCtlSrc::IOCtlSrc(std::string filename)
  : m_filename(std::move(filename))
{
}
IOCtlSrc::~IOCtlSrc() = default;
bool IOCtlSrc::Reopen(Error* error)
{
  Error::SetString(error, "Physical discs unsupported on Orbis");
  return false;
}
u32 IOCtlSrc::GetSectorCount() const
{
  return 0;
}
const std::vector<toc_entry>& IOCtlSrc::ReadTOC() const
{
  static const std::vector<toc_entry> empty;
  return empty;
}
bool IOCtlSrc::ReadSectors2048(u32 sector, u32 count, u8* buffer) const
{
  (void)sector;
  (void)count;
  (void)buffer;
  return false;
}
bool IOCtlSrc::ReadSectors2352(u32 sector, u32 count, u8* buffer) const
{
  (void)sector;
  (void)count;
  (void)buffer;
  return false;
}
bool IOCtlSrc::ReadTrackSubQ(cdvdSubQ* subq) const
{
  (void)subq;
  return false;
}
u32 IOCtlSrc::GetLayerBreakAddress() const
{
  return 0;
}
s32 IOCtlSrc::GetMediaType() const
{
  return 0;
}
bool IOCtlSrc::DiscReady()
{
  return false;
}
void GetValidDrive(std::string& drive)
{
  drive.clear();
}

static bool Unsupported(Error* error, const char* what)
{
  Error::SetString(error, what);
  return false;
}

CsoFileReader::CsoFileReader() = default;
CsoFileReader::~CsoFileReader() = default;
bool CsoFileReader::Open2(std::string filename, Error* error)
{
  (void)filename;
  return Unsupported(error, "CSO/ZSO not supported on Orbis (no zlib)");
}
bool CsoFileReader::Precache2(ProgressCallback* progress, Error* error)
{
  (void)progress;
  return Unsupported(error, "CSO/ZSO not supported on Orbis (no zlib)");
}
ThreadedFileReader::Chunk CsoFileReader::ChunkForOffset(u64 offset)
{
  (void)offset;
  return ThreadedFileReader::Chunk{-1, 0, 0};
}
int CsoFileReader::ReadChunk(void* dst, s64 chunkID)
{
  (void)dst;
  (void)chunkID;
  return -1;
}
void CsoFileReader::Close2()
{
}
u32 CsoFileReader::GetBlockCount() const
{
  return 0;
}

ChdFileReader::ChdFileReader() = default;
ChdFileReader::~ChdFileReader() = default;
bool ChdFileReader::Open2(std::string filename, Error* error)
{
  (void)filename;
  return Unsupported(error, "CHD not supported on Orbis (no libchdr)");
}
bool ChdFileReader::Precache2(ProgressCallback* progress, Error* error)
{
  (void)progress;
  return Unsupported(error, "CHD not supported on Orbis (no libchdr)");
}
ThreadedFileReader::Chunk ChdFileReader::ChunkForOffset(u64 offset)
{
  (void)offset;
  return ThreadedFileReader::Chunk{-1, 0, 0};
}
int ChdFileReader::ReadChunk(void* dst, s64 blockID)
{
  (void)dst;
  (void)blockID;
  return -1;
}
void ChdFileReader::Close2()
{
}
uint ChdFileReader::GetBlockCount() const
{
  return 0;
}

GzippedFileReader::GzippedFileReader() = default;
GzippedFileReader::~GzippedFileReader() = default;
bool GzippedFileReader::Open2(std::string filename, Error* error)
{
  (void)filename;
  return Unsupported(error, "GZIP images not supported on Orbis (no zlib)");
}
ThreadedFileReader::Chunk GzippedFileReader::ChunkForOffset(u64 offset)
{
  (void)offset;
  return ThreadedFileReader::Chunk{-1, 0, 0};
}
int GzippedFileReader::ReadChunk(void* dst, s64 chunkID)
{
  (void)dst;
  (void)chunkID;
  return -1;
}
void GzippedFileReader::Close2()
{
}
u32 GzippedFileReader::GetBlockCount() const
{
  return 0;
}
