/*
    Write Once Read Many database table backend
    Copyright (C) 2013    Morten Hustveit
    Copyright (C) 2013    eVenture Capital Partners II

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "src/table-backend-writeonce.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include <err.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sysexits.h>
#include <unistd.h>

#include <kj/debug.h>

#include <zstd.h>

#include "src/ca-table.h"
#include "src/util.h"

#include "third_party/oroch/oroch/integer_codec.h"

#define MAGIC UINT64_C(0x6c6261742e692e70)
#define MAJOR_VERSION 4
#define MINOR_VERSION 0

#define TMP_SUFFIX ".tmp.XXXXXX"
#define BUFFER_SIZE (1024 * 1024)

namespace cantera {
namespace table {

using namespace internal;

namespace {

static constexpr size_t kBlockSizeMin = 16 * 1024;
static constexpr size_t kBlockSizeMax = 2 * kBlockSizeMin - 1;

// The maximum entry size that is kept in normal blocks.
// Larger entries are kept in individual blocks.
static constexpr size_t kEntrySizeLimit = kBlockSizeMin / 2;

static const size_t kCompressedSizeMax = ZSTD_compressBound(kBlockSizeMax);

enum CA_wo_flags {
  CA_WO_FLAG_ASCENDING = 0x0001,
  CA_WO_FLAG_DESCENDING = 0x0002
};

struct CA_wo_header {
  uint64_t magic;
  uint8_t major_version;
  uint8_t minor_version;
  uint16_t flags;
  uint8_t compression;
  uint8_t compression_level;
  uint16_t data_reserved;
  uint64_t index_offset;
};

class DataBuffer {
 public:
  using uchar = unsigned char;

  DataBuffer(size_t capacity = 0) { reserve(capacity); }

  DataBuffer(const DataBuffer&) = delete;
  DataBuffer& operator=(const DataBuffer&) = delete;

  size_t capacity() const { return capacity_; }

  size_t size() const { return size_; }

  char* data() { return data_.get(); }
  const char* data() const { return data_.get(); }

  uchar* udata() { return reinterpret_cast<uchar*>(data()); }
  const uchar* udata() const { return reinterpret_cast<const uchar*>(data()); }

  void clear() { size_ = 0; }

  void reserve(size_t capacity) {
    if (capacity <= capacity_) return;

    std::unique_ptr<char[]> tmp(new char[capacity]);
    if (size_) std::copy_n(data_.get(), size_, tmp.get());
    data_.swap(tmp);

    capacity_ = capacity;
  }

  void resize(size_t size) {
    reserve(size);
    size_ = size;
  }

  template <typename T>
  void append(const std::vector<T>& vector) {
    size_t offset = size();
    size_t length = vector.size() * sizeof(T);
    resize(offset + length);
    memcpy(data() + offset, vector.data(), length);
  }

 private:
  size_t size_ = 0;
  size_t capacity_ = 0;
  std::unique_ptr<char[]> data_;
};

class RandomIO {
 public:
  RandomIO(int fd) : fd_(fd) {}

  RandomIO(const RandomIO&) = delete;
  RandomIO& operator=(const RandomIO&) = delete;

  void Read(void* buffer, off_t offset, size_t length) {
    ssize_t n = pread(fd_, buffer, length, offset);
    if (n != length) {
      if (n < 0)
        KJ_FAIL_SYSCALL("pread", errno);
      else
        KJ_FAIL_REQUIRE("pread incomplete");
    }
  }

  void Write(const void* buffer, off_t offset, size_t length) {
    ssize_t n = pwrite(fd_, buffer, length, offset);
    if (n != length) {
      if (n < 0)
        KJ_FAIL_SYSCALL("pwrite", errno);
      else
        KJ_FAIL_REQUIRE("pwrite incomplete");
    }
  }

  void Read(DataBuffer& buffer, off_t offset) {
    Read(buffer.data(), offset, buffer.size());
  }

  void Write(const DataBuffer& buffer, off_t offset) {
    Write(buffer.data(), offset, buffer.size());
  }

 private:
  // File descriptor.
  int fd_;
};

class ZstdCompressor {
 public:
  void Go(DataBuffer& dst, const DataBuffer& src, int level) {
    size_t size = ZSTD_compressCCtx(context_.get(), dst.data(), dst.capacity(),
                                    src.data(), src.size(), level);
    if (ZSTD_isError(size))
      KJ_FAIL_REQUIRE("compression error", ZSTD_getErrorName(size));
    dst.resize(size);
  }

 private:
  // Compression context.
  typedef std::unique_ptr<ZSTD_CCtx, decltype(ZSTD_freeCCtx)*> ContextPtr;
  ContextPtr context_ = ContextPtr(CreateContext(), ZSTD_freeCCtx);

  static ZSTD_CCtx* CreateContext() {
    ZSTD_CCtx* ctx = ZSTD_createCCtx();
    if (ctx == NULL) KJ_FAIL_REQUIRE("out of memory");
    return ctx;
  }
};

class ZstdDecompressor {
 public:
  void Go(DataBuffer& dst, const DataBuffer& src) {
    size_t size = ZSTD_decompressDCtx(context_.get(), dst.data(),
                                      dst.capacity(), src.data(), src.size());
    if (ZSTD_isError(size))
      KJ_FAIL_REQUIRE("decompression error", ZSTD_getErrorName(size));
    dst.resize(size);
  }

 private:
  // Decompression context.
  using ContextPtr = std::unique_ptr<ZSTD_DCtx, decltype(ZSTD_freeDCtx)*>;
  ContextPtr context_ = ContextPtr(CreateContext(), ZSTD_freeDCtx);

  static ZSTD_DCtx* CreateContext() {
    ZSTD_DCtx* ctx = ZSTD_createDCtx();
    if (ctx == NULL) KJ_FAIL_REQUIRE("out of memory");
    return ctx;
  }
};

class WriteOnceBlock {
 public:
  bool Add(const string_view& key, const string_view& value) {
    size_t kts = key_data_.size() + key.size();
    size_t vts = value_data_.size() + value.size();
    if ((kts + vts) > kBlockSizeMax) return false;

    key_size_.push_back(key.size());
    key_data_.insert(key_data_.end(), key.begin(), key.end());

    value_size_.push_back(value.size());
    value_data_.insert(value_data_.end(), value.begin(), value.end());

    return true;
  }

  void Clear() {
    key_size_.clear();
    key_data_.clear();

    value_size_.clear();
    value_data_.clear();
  }

  void Marshal(DataBuffer& buffer) const {
    buffer.clear();

    size_t num = key_size_.size();
    if (!num) return;

    size_t size = key_data_.size() + value_data_.size();
    size += oroch::varint_codec<size_t>::value_space(num);
    size += 2 * num * sizeof(uint32_t);
    buffer.reserve(size);

    unsigned char* ptr = buffer.udata();
    oroch::varint_codec<size_t>::value_encode(ptr, num);
    buffer.resize(ptr - buffer.udata());
    buffer.append(key_size_);
    buffer.append(value_size_);
    buffer.append(key_data_);
    buffer.append(value_data_);
  }

  void Unmarshal(DataBuffer& buffer) { Clear(); }

 private:
  // Accumulated key data.
  std::vector<uint32_t> key_size_;
  std::vector<char> key_data_;

  // Accumulated value data.
  std::vector<uint32_t> value_size_;
  std::vector<char> value_data_;
};

class WriteOnceIndex {
 public:
  void Clear() {}

  void Add(uint32_t block_size, const string_view& last_key) {
    block_size_.push_back(block_size);

    key_size_.push_back(last_key.size());
    key_data_.insert(key_data_.end(), last_key.begin(), last_key.end());
  }

  void Marshal(DataBuffer& buffer) const {
    buffer.clear();

    size_t num = key_size_.size();
    if (!num) return;

    size_t size = key_data_.size();
    size += oroch::varint_codec<size_t>::value_space(num);
    size += 2 * num * sizeof(uint32_t);
    buffer.reserve(size);

    unsigned char* ptr = buffer.udata();
    oroch::varint_codec<size_t>::value_encode(ptr, num);
    buffer.resize(ptr - buffer.udata());
    buffer.append(block_size_);
    buffer.append(key_size_);
    buffer.append(key_data_);
  }

  void Unmarshal(DataBuffer& buffer) { Clear(); }

 private:
  // Block sizes.
  std::vector<uint32_t> block_size_;

  // Last key in a block data.
  std::vector<uint32_t> key_size_;
  std::vector<char> key_data_;
};

class WriteOnceBuilder {
 public:
  WriteOnceBuilder(const char* path, const TableOptions& options)
      : path_(path), options_(options) {
    compression_ = options.GetCompression();
    if (compression_ == kTableCompressionDefault)
      compression_ = kTableCompressionNone;
    KJ_REQUIRE(compression_ <= kTableCompressionLast,
               "unsupported compression method");

    std::string dir(".");
    if (const char* last_slash = strrchr(path, '/')) {
      KJ_REQUIRE(path != last_slash);
      dir = std::string(path, last_slash - 1);
    }

    raw_fd_ = AnonTemporaryFile(dir.c_str());

    raw_stream_ = fdopen(raw_fd_.get(), "w");
    if (!raw_stream_) KJ_FAIL_SYSCALL("fdopen", errno, path);

    index_.reserve(12 * 1024 * 1024);
  }

  ~WriteOnceBuilder() {
    raw_fd_ = nullptr;
    if (raw_stream_) fclose(raw_stream_);
  }

  void NoFSync(bool value = true) {
    no_fsync_ = value;
  }

  void Add(const string_view& key, const string_view& value) {
    AddEntry(key, value);
    WriteEntryData(key, value);
  }

  void Build() {
    FlushEntryData();
    SortEntries();

    CreateFile();
    try {
      WriteFinalData();
      CommitFile();
    } catch (...) {
      RemoveFile();
      throw;
    }
  }

 private:
  struct Entry {
    uint64_t offset;
    uint32_t value_size;
    uint32_t key_size;
    char prefix[24];
  };

  void AddEntry(const string_view& key, const string_view& value) {
    KJ_REQUIRE(key.size() <= std::numeric_limits<uint32_t>::max(),
               "too long key");
    KJ_REQUIRE(value.size() <= std::numeric_limits<uint32_t>::max(),
               "too long value");

    struct Entry entry;
    size_t count = std::min(sizeof entry.prefix, key.size());
    entry.offset = offset_;
    entry.key_size = key.size();
    entry.value_size = value.size();
    std::copy_n(key.begin(), count, entry.prefix);
    index_.push_back(entry);

    offset_ += key.size() + value.size();
    if (key_size_max_ < key.size()) key_size_max_ = key.size();
  }

  void WriteEntryData(const string_view& key, const string_view& value) {
    KJ_REQUIRE(raw_stream_ != nullptr);

    if (0 == fwrite(key.data(), key.size(), 1, raw_stream_) ||
        0 == fwrite(value.data(), value.size(), 1, raw_stream_))
      KJ_FAIL_SYSCALL("fwrite", errno);
  }

  void FlushEntryData() {
    KJ_REQUIRE(raw_stream_ != nullptr);
    int ret = fflush(raw_stream_);
    if (ret) KJ_FAIL_SYSCALL("fflush", errno);
  }

  bool Compare(const Entry& lhs, const Entry& rhs) {
    size_t lhs_count = std::min(sizeof lhs.prefix, size_t{lhs.key_size});
    size_t rhs_count = std::min(sizeof rhs.prefix, size_t{rhs.key_size});
    const string_view lhs_prefix(lhs.prefix, lhs_count);
    const string_view rhs_prefix(rhs.prefix, rhs_count);

    int fast_result = lhs_prefix.compare(rhs_prefix);
    if (fast_result) return fast_result < 0;

    RandomIO fd(raw_fd_.get());
    fd.Read(lhs_buffer_.get(), lhs.offset, lhs.key_size);
    fd.Read(rhs_buffer_.get(), rhs.offset, rhs.key_size);
    read_count_ += 2;

    string_view lhs_view(lhs_buffer_.get(), lhs.key_size);
    string_view rhs_view(rhs_buffer_.get(), rhs.key_size);
    return lhs_view < rhs_view;
  }

  void SortEntries() {
    lhs_buffer_ = std::make_unique<char[]>(key_size_max_);
    rhs_buffer_ = std::make_unique<char[]>(key_size_max_);

    // Use stable_sort here. It is usually implemented using merge sort
    // algorithm that is preferred over quick sort when disk access is
    // involved.
    std::stable_sort(index_.begin(), index_.end(),
                     [this](const auto& lhs, const auto& rhs) {
      return this->Compare(lhs, rhs);
    });

    KJ_LOG(DBG, index_.size(), read_count_);

    lhs_buffer_.reset();
    rhs_buffer_.reset();
  }

  void CreateFile() {
    char* temp = NULL;
    KJ_SYSCALL(asprintf(&temp, "%s.tmp.%u.XXXXXX", path_.c_str(), getpid()));
    std::unique_ptr<char[], decltype(free)*> temp_name_free(temp, free);

    int fd;
    KJ_SYSCALL(fd = mkstemp(temp), temp);
    fd_ = kj::AutoCloseFd(fd);
    tmp_path_ = std::string(temp);

    int nflags = options_.GetFileFlags();
    KJ_REQUIRE((nflags & ~(O_CLOEXEC)) == 0);

    int oflags;
    KJ_SYSCALL(oflags = fcntl(fd, F_GETFD));
    if ((oflags & nflags) != nflags)
      KJ_SYSCALL(fcntl(fd, F_SETFD, oflags | nflags));

    struct CA_wo_header header;
    header.magic = MAGIC;  // Will implicitly store endianness
    header.major_version = MAJOR_VERSION;
    header.minor_version = MINOR_VERSION;
    header.flags = CA_WO_FLAG_ASCENDING;
    header.compression = compression_;
    header.compression_level = options_.GetCompressionLevel();
    header.data_reserved = 0;
    //header.index_offset = (write_offset + 0xfff) & ~0xfffULL;

    kj::FdOutputStream output(fd);
    output.write(&header, sizeof(header));
  }

  void CommitFile() {
    auto mode = options_.GetFileMode();
    auto mask = umask(0);
    umask(mask);

    KJ_SYSCALL(fchmod(fd_.get(), mode & ~mask), tmp_path_);
    KJ_SYSCALL(rename(tmp_path_.c_str(), path_.c_str()), tmp_path_, path_);

    if (!no_fsync_) {
      // TODO(mortehu): fsync all ancestor directories too.
      KJ_SYSCALL(fsync(fd_.get()), path_);
    }
  }

  void RemoveFile() {
    unlink(tmp_path_.c_str());
  }

  void WriteFinalData() {
    WriteOnceIndex index;
    WriteOnceBlock block;

    RandomIO raw_fd(raw_fd_.get());
    DataBuffer buffer(kEntrySizeLimit);
    std::string last_key;

    size_t block_count = 0, large_count = 0;

    for (const Entry& entry : index_) {
      size_t size = entry.key_size + entry.value_size;
      if (size > kEntrySizeLimit) {
        // TODO: store large entries separately
        large_count++;
        continue;
      }

      buffer.resize(size);
      raw_fd.Read(buffer, entry.offset);

      const char* data = buffer.data();
      const string_view key(data, entry.key_size);
      const string_view value(data + entry.key_size, entry.value_size);

      if (!block.Add(key, value)) {
        WriteBlock(block, index, last_key);
        block.Clear();
        block_count++;

        if (!block.Add(key, value))
          KJ_FAIL_REQUIRE("an entry does not fit a block");
      }

      last_key.assign(key.data(), key.size());
    }

    if (WriteBlock(block, index, last_key))
      block_count++;
    WriteIndex(index);

    KJ_LOG(DBG, block_count, large_count);
  }

  bool WriteBlock(const WriteOnceBlock& block, WriteOnceIndex& index,
                  const std::string &last_key) {
    block.Marshal(write_buffer_);
    if (!write_buffer_.size())
      return false;

    index.Add(write_buffer_.size(), string_view(last_key));

    Write(write_buffer_);
    return true;
  }

  bool WriteIndex(const WriteOnceIndex& index) {
    index.Marshal(write_buffer_);
    if (!write_buffer_.size())
      return false;

    Write(write_buffer_);
    return true;
  }

  void Write(const DataBuffer& buffer) {
    ssize_t n = write(fd_.get(), buffer.data(), buffer.size());
    if (n != buffer.size()) {
      if (n < 0)
        KJ_FAIL_SYSCALL("write", errno);
      else
        KJ_FAIL_REQUIRE("write incomplete");
    }
  }

  // Final file path.
  const std::string path_;
  // Temporary file name.
  std::string tmp_path_;

  // Saved table creation options.
  const TableOptions options_;
  TableCompression compression_ = TableCompression::kTableCompressionNone;

  // Whether to fsync the data.
  bool no_fsync_ = false;

  // Result file.
  kj::AutoCloseFd fd_;

  // Temporary file for all added key/value pairs.
  kj::AutoCloseFd raw_fd_;
  FILE* raw_stream_ = nullptr;

  // Index of all added key/value pairs.
  std::vector<Entry> index_;

  // Running file offset.
  uint64_t offset_ = 0;

  // Longest encountered key size.
  uint32_t key_size_max_ = 0;
  uint32_t entry_size_max_ = 0;

  // Buffers for key comparison.
  std::unique_ptr<char[]> lhs_buffer_;
  std::unique_ptr<char[]> rhs_buffer_;

  // A buffer for block marshaling and writing.
  DataBuffer write_buffer_;

  // Read statistics.
  uint64_t read_count_ = 0;
};

class WriteOnceTable : public SeekableTable {
 public:
  WriteOnceTable(const char* path, const TableOptions& options);
  WriteOnceTable(const char* path);

  ~WriteOnceTable();

  void Sync() override;

  void SetFlag(enum ca_table_flag flag) override;

  int IsSorted() override;

  void InsertRow(const struct iovec* value, size_t value_count) override;

  void Seek(off_t offset, int whence) override;

  void SeekToFirst() override { Seek(0, SEEK_SET); }

  bool SeekToKey(const string_view& key) override;

  off_t Offset() override;

  bool ReadRow(struct iovec* key, struct iovec* value) override;

 private:
  friend class WriteOnceTableBackend;

  void FlushKeyBuffer();

  void MAdviseIndex();

  void BuildIndex();

  void MemoryMap();

  std::string path;

  int fd = -1;
  int open_flags = 0;

  TableCompression compression_ = TableCompression::kTableCompressionNone;

  uint64_t write_offset = 0;

  void* buffer = MAP_FAILED;
  size_t buffer_size = 0, buffer_fill = 0;

  struct CA_wo_header* header = nullptr;

  uint64_t entry_count = 0;

  union {
    uint64_t* u64;
    uint32_t* u32;
    uint16_t* u16;
  } index;
  uint64_t index_size = 0;
  unsigned int index_bits = 0;

  bool has_madvised_index = false;

  int no_relative = 0;

  // Used for read, seek, offset and delete.
  uint64_t offset_ = 0;

  // Unflushed hash map keys.
  std::vector<std::pair<uint64_t, uint64_t>> key_buffer;

  // Table builder.
  std::unique_ptr<WriteOnceBuilder> builder_;
};

uint64_t CA_wo_hash(const string_view& str) {
  auto result = UINT64_C(0x2257d6803a6f1b2);

  for (auto ch : str) result = result * 31 + static_cast<unsigned char>(ch);

  return result;
}

WriteOnceTable::WriteOnceTable(const char* path, const TableOptions& options)
    : path(path) {
  int flags = options.GetFileFlags();
  KJ_REQUIRE((flags & ~(O_EXCL | O_CLOEXEC)) == 0);
  flags |= O_CREAT | O_TRUNC | O_RDWR;

  open_flags = flags;

  builder_ = std::make_unique<WriteOnceBuilder>(path, options);
}

WriteOnceTable::WriteOnceTable(const char* path) : path(path) {
  open_flags = O_RDONLY | O_CLOEXEC;

  KJ_SYSCALL(fd = open(path, open_flags));

  MemoryMap();

  KJ_REQUIRE(header != nullptr);
  compression_ = TableCompression(header->compression);
  KJ_REQUIRE(compression_ <= kTableCompressionLast,
             "unsupported compression method");

  if (compression_ != TableCompression::kTableCompressionNone)
    KJ_UNIMPLEMENTED("decompression is not implemented yet");
}

}  // namespace

std::unique_ptr<Table> WriteOnceTableBackend::Create(
    const char* path, const TableOptions& options) {
  return std::make_unique<WriteOnceTable>(path, options);
}

std::unique_ptr<Table> WriteOnceTableBackend::Open(const char* path) {
  return std::make_unique<WriteOnceTable>(path);
}

std::unique_ptr<SeekableTable> WriteOnceTableBackend::OpenSeekable(
    const char* path) {
  return std::make_unique<WriteOnceTable>(path);
}

WriteOnceTable::~WriteOnceTable() {
  if (fd != -1) close(fd);

  if (buffer != MAP_FAILED) munmap(buffer, buffer_size);
}

void WriteOnceTable::FlushKeyBuffer() {
  MAdviseIndex();

  std::sort(key_buffer.begin(), key_buffer.end());

  for (auto& hash_offset : key_buffer) {
    uint64_t hash = hash_offset.first;

    while (index.u64[hash]) {
      if (++hash == index_size) hash = 0;
    }

    index.u64[hash] = hash_offset.second;
  }

  key_buffer.clear();
}

void WriteOnceTable::MAdviseIndex() {
  auto base = reinterpret_cast<ptrdiff_t>(buffer) + header->index_offset;
  auto end = reinterpret_cast<ptrdiff_t>(buffer) + buffer_fill;
  base &= ~0xfff;

  KJ_SYSCALL(madvise(reinterpret_cast<void*>(base), end - base, MADV_WILLNEED),
             base, end, (ptrdiff_t)buffer, buffer_size);
  has_madvised_index = true;
}

void WriteOnceTable::BuildIndex() {
  static const uint64_t kKeyBufferMax = 16 * 1024 * 1024;
  struct iovec key_iov, value;
  std::string prev_key, key;
  unsigned int flags = CA_WO_FLAG_ASCENDING | CA_WO_FLAG_DESCENDING;

  Seek(0, SEEK_SET);

  printf("entry_count: %llu\n", entry_count);
  key_buffer.reserve(std::min(entry_count, kKeyBufferMax));

  KJ_SYSCALL(madvise(buffer, header->index_offset, MADV_SEQUENTIAL));

  for (;;) {
    uint64_t hash;

    int cmp;

    auto tmp_offset = offset_;

    if (!ReadRow(&key_iov, &value)) break;

    key.assign(reinterpret_cast<const char*>(key_iov.iov_base),
               key_iov.iov_len);

    if (flags && !prev_key.empty()) {
      cmp = prev_key.compare(key);

      if (cmp < 0) {
        flags &= CA_WO_FLAG_ASCENDING;
      } else if (cmp > 0) {
        flags &= CA_WO_FLAG_DESCENDING;
      }
    }

    if (header->major_version < 2) {
      hash = CA_wo_hash(key);
    } else {
      hash = Hash(key);
    }

    hash %= index_size;

    prev_key.swap(key);

    key_buffer.emplace_back(hash, tmp_offset);

    if (key_buffer.size() >= kKeyBufferMax) {
      // Discard the keys we've already read.
      if (0 != (tmp_offset & ~0xfff)) {
        KJ_SYSCALL(madvise(buffer, tmp_offset & ~0xfff, MADV_DONTNEED));
      }

      FlushKeyBuffer();
    }
  }

  FlushKeyBuffer();

  header->flags = flags;
}

void WriteOnceTable::Sync() {
  if (builder_)
    builder_->Build();
}

void WriteOnceTable::SetFlag(enum ca_table_flag flag) {
  switch (flag) {
    case CA_TABLE_NO_RELATIVE:
      no_relative = 1;
      break;

    case CA_TABLE_NO_FSYNC:
      if (builder_)
        builder_->NoFSync();
      break;

    default:
      KJ_FAIL_REQUIRE("unknown flag", flag);
  }
}

int WriteOnceTable::IsSorted() {
  return 0 != (header->flags & CA_WO_FLAG_ASCENDING);
}

void WriteOnceTable::InsertRow(const struct iovec* value, size_t value_count) {
  KJ_REQUIRE(value_count == 2);

  if (builder_) {
    const string_view k(reinterpret_cast<const char*>(value[0].iov_base),
                        value[0].iov_len);
    const string_view v(reinterpret_cast<const char*>(value[1].iov_base),
                        value[1].iov_len);
    builder_->Add(k, v);
  }
}

void WriteOnceTable::Seek(off_t rel_offset, int whence) {
  uint64_t new_offset;

  switch (whence) {
    case SEEK_SET:
      KJ_REQUIRE(rel_offset >= 0);
      new_offset = sizeof(struct CA_wo_header) + rel_offset;
      break;

    case SEEK_CUR:
      new_offset = offset_ + rel_offset;
      break;

    case SEEK_END:
      KJ_REQUIRE(rel_offset <= 0);
      new_offset = header->index_offset - rel_offset;
      break;

    default:
      KJ_FAIL_REQUIRE(!"Invalid 'whence' value");
  }

  KJ_REQUIRE(new_offset >= sizeof(struct CA_wo_header),
             "attempt to seek before start of table");

  KJ_REQUIRE(new_offset <= header->index_offset,
             "attempt to seek past end of table");

  offset_ = new_offset;
}

bool WriteOnceTable::SeekToKey(const string_view& key) {
  if (!has_madvised_index) MAdviseIndex();

  uint64_t hash, tmp_offset;

  if (header->major_version < 2) {
    hash = CA_wo_hash(key);
  } else {
    hash = Hash(key);
  }

  uint64_t min_offset = 0;
  uint64_t max_offset = buffer_fill;

  hash %= index_size;

  uint64_t fib[2] = {2, 1};
  unsigned int collisions = 0;

  for (;;) {
    switch (index_bits) {
      case 16:
        tmp_offset = index.u16[hash];
        break;
      case 32:
        tmp_offset = index.u32[hash];
        break;
      default:
        tmp_offset = index.u64[hash];
        break;
    }

    if (!tmp_offset) return false;

    if (tmp_offset >= min_offset && tmp_offset <= max_offset) {
      auto data = reinterpret_cast<const char*>(buffer) + tmp_offset;

      while ((*data) & 0x80) ++data;
      ++data;

      auto cmp = key.compare(data);

      if (cmp == 0) {
        offset_ = tmp_offset;
        return true;
      } else if (cmp < 0) {
        if (0 != (header->flags & CA_WO_FLAG_ASCENDING))
          max_offset = tmp_offset;
      } else {
        if (0 != (header->flags & CA_WO_FLAG_ASCENDING))
          min_offset = tmp_offset;
      }
    }

    if (header->major_version >= 3) {
      if (++hash == index_size) hash = 0;
    } else {
      ++collisions;
      hash = (hash + fib[collisions & 1]) % index_size;
      fib[collisions & 1] += fib[~collisions & 1];
    }
  }

  return false;
}

off_t WriteOnceTable::Offset() { return offset_ - sizeof(struct CA_wo_header); }

bool WriteOnceTable::ReadRow(struct iovec* key, struct iovec* value) {
  uint64_t size;
  uint8_t* p;

  KJ_REQUIRE(offset_ >= sizeof(struct CA_wo_header));

  p = reinterpret_cast<uint8_t*>(buffer) + offset_;

  if (offset_ >= header->index_offset || *p == 0) return false;

  size = ca_parse_integer((const uint8_t**)&p);

  key->iov_base = p;
  key->iov_len = strlen(reinterpret_cast<const char*>(key->iov_base));

  KJ_ASSERT(size > key->iov_len, size, key->iov_len);

  value->iov_base = p + key->iov_len + 1;
  value->iov_len = size - key->iov_len - 1;

  p += size;
  offset_ = p - reinterpret_cast<uint8_t*>(buffer);

  return true;
}

/*****************************************************************************/

void WriteOnceTable::MemoryMap() {
  off_t end = 0;
  int prot = PROT_READ;

  KJ_SYSCALL(end = lseek(fd, 0, SEEK_END), path);

  KJ_REQUIRE(static_cast<size_t>(end) > sizeof(struct CA_wo_header), end,
             sizeof(struct CA_wo_header));

  buffer_size = end;
  buffer_fill = end;

  if ((open_flags & O_RDWR) == O_RDWR) prot |= PROT_WRITE;

  if (MAP_FAILED == (buffer = mmap(NULL, end, prot, MAP_SHARED, fd, 0))) {
    KJ_FAIL_SYSCALL("mmap", errno, path);
  }

  header = (struct CA_wo_header*)buffer;

  KJ_REQUIRE(header->major_version <= MAJOR_VERSION ||
             header->major_version >= 2);

  KJ_REQUIRE(header->magic == MAGIC, header->magic, MAGIC);

  if (header->major_version >= 3)
    index_bits = 64;
  else if (!(header->index_offset & ~0xffffULL))
    index_bits = 16;
  else if (!(header->index_offset & ~0xffffffffULL))
    index_bits = 32;
  else
    index_bits = 64;

  index.u64 = reinterpret_cast<uint64_t*>(
      (reinterpret_cast<char*>(buffer) + header->index_offset));
  index_size = (buffer_size - header->index_offset) / (index_bits / CHAR_BIT);

  offset_ = sizeof(struct CA_wo_header);
}

}  // namespace table
}  // namespace cantera
