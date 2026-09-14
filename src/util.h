#pragma once

#include <string>
#include <vector>
#include <cstddef>

template <typename T, size_t N>
size_t array_size(T (&)[N]) {
  return N;
}

std::vector<char*> MakeArgV(std::vector<std::string>* args);

bool ReadFully(int fd, void* data, size_t size);
void WriteFully(int fd, const void* data, size_t size);
