#pragma once

#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace graph_engine {
namespace storage {

class MmapFile {
public:
  MmapFile() : data_(nullptr), size_(0), fd_(-1) {}

  ~MmapFile() { close(); }

  // Non-copyable
  MmapFile(const MmapFile &) = delete;
  MmapFile &operator=(const MmapFile &) = delete;

  // Movable
  MmapFile(MmapFile &&other) noexcept
      : data_(other.data_), size_(other.size_), fd_(other.fd_) {
    other.data_ = nullptr;
    other.size_ = 0;
    other.fd_ = -1;
  }

  MmapFile &operator=(MmapFile &&other) noexcept {
    if (this != &other) {
      close();
      data_ = other.data_;
      size_ = other.size_;
      fd_ = other.fd_;
      other.data_ = nullptr;
      other.size_ = 0;
      other.fd_ = -1;
    }
    return *this;
  }

  bool open(const std::string &path) {
    close();

    fd_ = ::open(path.c_str(), O_RDONLY);
    if (fd_ < 0)
      return false;

    struct stat sb;
    if (::fstat(fd_, &sb) == -1) {
      close();
      return false;
    }

    size_ = sb.st_size;
    if (size_ == 0) {
      // Empty file is a valid edge case but mmap fails on 0 bytes.
      return true;
    }

    // Map the file into memory (Read Only, Private mapping for protection)
    data_ = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
    if (data_ == MAP_FAILED) {
      data_ = nullptr;
      size_ = 0;
      close();
      return false;
    }

    // Advise the kernel we will read this pseudo-randomly but want it cached
    // to improve page-fault latency during graph walks.
    ::madvise(data_, size_, MADV_RANDOM);

    return true;
  }

  void close() {
    if (data_ && data_ != MAP_FAILED) {
      ::munmap(data_, size_);
      data_ = nullptr;
    }
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
    size_ = 0;
  }

  const uint8_t *data() const { return static_cast<const uint8_t *>(data_); }
  size_t size() const { return size_; }
  bool is_open() const {
    return fd_ >= 0 || (size_ == 0 && data_ == nullptr && fd_ < 0);
  } // handle empty file logic

  // Helper cast for flat arrays
  template <typename T> const T *as_array() const {
    return reinterpret_cast<const T *>(data_);
  }

private:
  void *data_;
  size_t size_;
  int fd_;
};

} // namespace storage
} // namespace graph_engine
