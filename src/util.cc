#include "util.h"
#include <errno.h>
#include <unistd.h>

std::vector<char*> MakeArgV(std::vector<std::string>* args) {
  std::vector<char*> argv;
  argv.reserve(args->size() + 1);
  for (auto& arg : *args)
    argv.push_back(arg.data());
  argv.push_back(nullptr);
  return argv;
}

bool ReadFully(int fd, void* data, size_t size) {
  char* p = reinterpret_cast<char*>(data);
  size_t to_read = size;
  while (to_read > 0) {
    ssize_t res = read(fd, p, to_read);
    if (res <= 0) {
      if (res < 0 && errno == EINTR)
        continue;
      return false;
    }
    p += res;
    to_read -= res;
  }
  return true;
}

void WriteFully(int fd, const void* data, size_t size) {
  const char* p = static_cast<const char*>(data);
  size_t to_write = size;
  while (to_write > 0) {
    ssize_t res = write(fd, p, to_write);
    if (res < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (res == 0)
      break;
    p += res;
    to_write -= res;
  }
}

unique_fd::~unique_fd() {
  reset();
}

void unique_fd::reset(int new_fd) {
  if (fd_ >= 0)
    close(fd_);
  fd_ = new_fd;
}
