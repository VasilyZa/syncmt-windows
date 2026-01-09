#include "file_operations.h"
#include <algorithm>
#include <vector>

size_t get_optimal_block_size() {
    SYSTEM_INFO sys_info;
    GetSystemInfo(&sys_info);
    size_t page_size = sys_info.dwPageSize;
    size_t granularity = sys_info.dwAllocationGranularity;
    return std::max(page_size, granularity) * 4;
}

bool is_disk_space_available(const fs::path& path, uintmax_t required_size) {
    ULARGE_INTEGER free_bytes_available, total_bytes, total_free_bytes;
    std::wstring root = path.root_path().wstring();
    if (GetDiskFreeSpaceExW(root.c_str(), &free_bytes_available, &total_bytes, &total_free_bytes)) {
        return free_bytes_available.QuadPart >= required_size;
    }
    return false;
}

uintmax_t get_file_size(const fs::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA file_info;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &file_info)) {
        LARGE_INTEGER size;
        size.HighPart = file_info.nFileSizeHigh;
        size.LowPart = file_info.nFileSizeLow;
        return static_cast<uintmax_t>(size.QuadPart);
    }
    return 0;
}

bool file_exists(const fs::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA file_info;
    return GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &file_info);
}

bool is_directory_path(const fs::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA file_info;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &file_info)) {
        return (file_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    return false;
}

bool create_directory_recursive(const fs::path& path) {
    std::error_code ec;
    return fs::create_directories(path, ec);
}

void ensure_directory_exists(const fs::path& path, std::unordered_set<fs::path>& created_dirs) {
    if (created_dirs.find(path) == created_dirs.end()) {
        if (create_directory_recursive(path)) {
            created_dirs.insert(path);
        }
    }
}

std::vector<fs::path> list_directory(const fs::path& path) {
    std::vector<fs::path> result;
    WIN32_FIND_DATAW find_data;
    std::wstring search_pattern = (path / L"*").wstring();
    
    FindHandle find_handle(FindFirstFileW(search_pattern.c_str(), &find_data));
    if (find_handle.get() == INVALID_HANDLE_VALUE) {
        return result;
    }
    
    do {
        const std::wstring filename = find_data.cFileName;
        if (filename != L"." && filename != L"..") {
            result.push_back(path / filename);
        }
    } while (FindNextFileW(find_handle.get(), &find_data));
    
    return result;
}

bool delete_file(const fs::path& path) {
    return DeleteFileW(path.c_str()) != 0;
}

bool delete_directory(const fs::path& path) {
    return RemoveDirectoryW(path.c_str()) != 0;
}

bool delete_directory_recursive(const fs::path& path) {
    if (!is_directory_path(path)) {
        return false;
    }
    
    for (const auto& entry : list_directory(path)) {
        if (is_directory_path(entry)) {
            if (!delete_directory_recursive(entry)) {
                return false;
            }
        } else {
            if (!delete_file(entry)) {
                return false;
            }
        }
    }
    
    return delete_directory(path);
}

bool compare_files(const fs::path& file1, const fs::path& file2) {
    uintmax_t size1 = get_file_size(file1);
    uintmax_t size2 = get_file_size(file2);
    
    if (size1 != size2) {
        return false;
    }
    
    FileHandle h1(CreateFileW(file1.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    FileHandle h2(CreateFileW(file2.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    
    if (h1.get() == INVALID_HANDLE_VALUE || h2.get() == INVALID_HANDLE_VALUE) {
        return false;
    }
    
    size_t block_size = get_optimal_block_size();
    std::vector<char> buffer1(block_size);
    std::vector<char> buffer2(block_size);
    
    DWORD bytes_read1, bytes_read2;
    while (true) {
        if (!ReadFile(h1.get(), buffer1.data(), static_cast<DWORD>(block_size), &bytes_read1, nullptr)) {
            return false;
        }
        if (!ReadFile(h2.get(), buffer2.data(), static_cast<DWORD>(block_size), &bytes_read2, nullptr)) {
            return false;
        }
        if (bytes_read1 != bytes_read2) {
            return false;
        }
        if (bytes_read1 == 0) {
            break;
        }
        if (memcmp(buffer1.data(), buffer2.data(), bytes_read1) != 0) {
            return false;
        }
    }
    
    return true;
}

bool copy_file_attributes(const fs::path& src, const fs::path& dst) {
    WIN32_FILE_ATTRIBUTE_DATA file_info;
    if (!GetFileAttributesExW(src.c_str(), GetFileExInfoStandard, &file_info)) {
        return false;
    }
    
    if (!SetFileAttributesW(dst.c_str(), file_info.dwFileAttributes)) {
        return false;
    }
    
    FileHandle src_handle(CreateFileW(src.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    FileHandle dst_handle(CreateFileW(dst.c_str(), GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    
    if (src_handle.get() == INVALID_HANDLE_VALUE || dst_handle.get() == INVALID_HANDLE_VALUE) {
        return false;
    }
    
    FILETIME creation_time, access_time, write_time;
    if (GetFileTime(src_handle.get(), &creation_time, &access_time, &write_time)) {
        SetFileTime(dst_handle.get(), &creation_time, &access_time, &write_time);
    }
    
    return true;
}
