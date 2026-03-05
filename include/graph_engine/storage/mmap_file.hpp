#pragma once

#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace graph_engine {
namespace storage {

class MmapFile {
public:
  MmapFile() = default;
  ~MmapFile() { close(); }

  // Non-copyable
  MmapFile(const MmapFile &) = delete;
  MmapFile &operator=(const MmapFile &) = delete;

  // Movable
  MmapFile(MmapFile &&other) noexcept
      : data_(other.data_), size_(other.size_), fd_(other.fd_),
        opened_(other.opened_) {
    other.data_ = nullptr;
    other.size_ = 0;
    other.fd_ = -1;
    other.opened_ = false;
  }

  MmapFile &operator=(MmapFile &&other) noexcept {
    if (this != &other) {
      close();
      data_ = other.data_;
      size_ = other.size_;
      fd_ = other.fd_;
      opened_ = other.opened_;
      other.data_ = nullptr;
      other.size_ = 0;
      other.fd_ = -1;
      other.opened_ = false;
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
      opened_ = true; // 空文件合法
      return true;
    }

    data_ = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
    if (data_ == MAP_FAILED) {
      data_ = nullptr;
      size_ = 0;
      close();
      return false;
    }

    ::madvise(data_, size_, MADV_RANDOM);
    opened_ = true;
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
    opened_ = false;
  }

  const uint8_t *data() const { return static_cast<const uint8_t *>(data_); }
  size_t size() const { return size_; }
  bool is_open() const { return opened_; }

  template <typename T> const T *as_array() const {
    return reinterpret_cast<const T *>(data_);
  }

private:
  void *data_{nullptr};
  size_t size_{0};
  int fd_{-1};
  bool opened_{false};
};

} // namespace storage
} // namespace graph_engine
