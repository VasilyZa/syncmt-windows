#pragma once

#include <filesystem>
#include <unordered_set>
#include <string>
#include <windows.h>
#include <system_error>
#include "file_handle.h"

namespace fs = std::filesystem;

size_t get_optimal_block_size();
bool is_disk_space_available(const fs::path& path, uintmax_t required_size);
uintmax_t get_file_size(const fs::path& path);
bool file_exists(const fs::path& path);
bool is_directory_path(const fs::path& path);
bool create_directory_recursive(const fs::path& path);
void ensure_directory_exists(const fs::path& path, std::unordered_set<fs::path>& created_dirs);
std::vector<fs::path> list_directory(const fs::path& path);
bool delete_file(const fs::path& path);
bool delete_directory(const fs::path& path);
bool delete_directory_recursive(const fs::path& path);
bool compare_files(const fs::path& file1, const fs::path& file2);
bool copy_file_attributes(const fs::path& src, const fs::path& dst);
