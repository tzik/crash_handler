#pragma once

#include <cstddef>
#include <string>
#include <vector>

std::vector<char*> MakeArgV(std::vector<std::string>* args);

bool ReadFully(int fd, void* data, size_t size);
void WriteFully(int fd, const void* data, size_t size);
