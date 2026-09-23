#pragma once

#include <cstddef>
#include <string>
#include <vector>

std::vector<char*> MakeArgV(std::vector<std::string>* args);

bool ReadFully(int fd, void* data, size_t size);
void WriteFully(int fd, const void* data, size_t size);

class unique_fd {
 public:
  unique_fd() = default;
  explicit unique_fd(int fd) : fd_(fd) {}
  ~unique_fd();

  unique_fd(const unique_fd&) = delete;
  unique_fd& operator=(const unique_fd&) = delete;

  unique_fd(unique_fd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
  unique_fd& operator=(unique_fd&& other) noexcept {
    if (this != &other)
      reset(other.release());
    return *this;
  }

  int get() const { return fd_; }
  bool is_valid() const { return fd_ >= 0; }

  int release() {
    int fd = fd_;
    fd_ = -1;
    return fd;
  }

  void reset(int new_fd = -1);

 private:
  int fd_ = -1;
};
