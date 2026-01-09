#pragma once

#include <filesystem>
#include <unordered_set>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <iostream>
#include <windows.h>
#include "thread_pool.h"
#include "file_operations.h"
#include "i18n.h"

namespace fs = std::filesystem;

enum class CopyMode {
    MEMORY_MAPPED,
    ASYNC_IO,
    STANDARD
};

class FileCopier {
private:
    ThreadPool thread_pool;
    CopyMode copy_mode;
    std::atomic<uintmax_t> total_copied;
    std::atomic<uintmax_t> total_files;
    std::atomic<uintmax_t> total_dirs;
    std::atomic<uintmax_t> total_errors;
    std::mutex error_mutex;
    std::vector<std::string> errors;
    
    void copy_file(const fs::path& src, const fs::path& dst, std::unordered_set<fs::path>& created_dirs) {
        try {
            if (compare_files(src, dst)) {
                return;
            }
            
            switch (copy_mode) {
                case CopyMode::MEMORY_MAPPED:
                    ThreadPool::copy_file_mmap(src, dst, created_dirs);
                    break;
                case CopyMode::ASYNC_IO:
                    ThreadPool::copy_file_async(src, dst, created_dirs);
                    break;
                case CopyMode::STANDARD:
                    copy_file_standard(src, dst, created_dirs);
                    break;
            }
            
            total_copied += get_file_size(src);
            total_files++;
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(error_mutex);
            errors.push_back(_("error_copying", src.string(), dst.string()) + ": " + e.what());
            total_errors++;
        }
    }
    
    void copy_file_standard(const fs::path& src, const fs::path& dst, std::unordered_set<fs::path>& created_dirs) {
        ensure_directory_exists(dst.parent_path(), created_dirs);
        
        FileHandle src_handle(CreateFileW(src.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (src_handle.get() == INVALID_HANDLE_VALUE) {
            throw std::runtime_error(_("error_open_src", src.string()));
        }
        
        uintmax_t file_size = get_file_size(src);
        if (file_size == 0) {
            FileHandle dst_handle(CreateFileW(dst.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (dst_handle.get() == INVALID_HANDLE_VALUE) {
                throw std::runtime_error(_("error_create_dst", dst.string()));
            }
            return;
        }
        
        if (!is_disk_space_available(dst.parent_path(), file_size)) {
            throw std::runtime_error(_("error_disk_space", dst.string()));
        }
        
        FileHandle dst_handle(CreateFileW(dst.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (dst_handle.get() == INVALID_HANDLE_VALUE) {
            throw std::runtime_error(_("error_create_dst", dst.string()));
        }
        
        size_t block_size = get_optimal_block_size();
        std::vector<char> buffer(block_size);
        
        DWORD bytes_read, bytes_written;
        while (true) {
            if (!ReadFile(src_handle.get(), buffer.data(), static_cast<DWORD>(block_size), &bytes_read, nullptr)) {
                throw std::runtime_error(_("error_read", src.string()));
            }
            if (bytes_read == 0) {
                break;
            }
            if (!WriteFile(dst_handle.get(), buffer.data(), bytes_read, &bytes_written, nullptr)) {
                throw std::runtime_error(_("error_write", dst.string()));
            }
            if (bytes_written != bytes_read) {
                throw std::runtime_error(_("error_incomplete", dst.string()));
            }
        }
        
        copy_file_attributes(src, dst);
    }
    
    void copy_directory(const fs::path& src, const fs::path& dst, std::unordered_set<fs::path>& created_dirs) {
        try {
            ensure_directory_exists(dst, created_dirs);
            total_dirs++;
            
            for (const auto& entry : list_directory(src)) {
                fs::path relative_path = fs::relative(entry, src);
                fs::path dst_path = dst / relative_path;
                
                if (is_directory_path(entry)) {
                    copy_directory(entry, dst_path, created_dirs);
                } else {
                    thread_pool.enqueue([this, entry, dst_path, &created_dirs]() {
                        copy_file(entry, dst_path, created_dirs);
                    });
                }
            }
            
            thread_pool.wait_for_tasks();
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(error_mutex);
            errors.push_back(_("error_dir", src.string()) + ": " + e.what());
            total_errors++;
        }
    }
    
public:
    FileCopier(size_t num_threads = std::thread::hardware_concurrency(), CopyMode mode = CopyMode::MEMORY_MAPPED)
        : thread_pool(num_threads), copy_mode(mode), total_copied(0), total_files(0), total_dirs(0), total_errors(0) {}
    
    void copy(const fs::path& src, const fs::path& dst) {
        if (!file_exists(src)) {
            throw std::runtime_error("Source path does not exist: " + src.string());
        }
        
        std::unordered_set<fs::path> created_dirs;
        
        if (is_directory_path(src)) {
            copy_directory(src, dst, created_dirs);
        } else {
            copy_file(src, dst, created_dirs);
        }
    }
    
    void set_copy_mode(CopyMode mode) {
        copy_mode = mode;
    }
    
    uintmax_t get_total_copied() const {
        return total_copied.load();
    }
    
    uintmax_t get_total_files() const {
        return total_files.load();
    }
    
    uintmax_t get_total_dirs() const {
        return total_dirs.load();
    }
    
    uintmax_t get_total_errors() const {
        return total_errors.load();
    }
    
    std::vector<std::string> get_errors() const {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(error_mutex));
        return errors;
    }
    
    void print_summary() const {
        std::cout << "\n" << _("summary") << "\n";
        std::cout << "  " << _("files_copied", std::to_string(get_total_files())) << "\n";
        std::cout << "  " << _("dirs_created", std::to_string(get_total_dirs())) << "\n";
        std::cout << "  " << _("bytes_copied", std::to_string(get_total_copied())) << "\n";
        std::cout << "  " << _("errors", std::to_string(get_total_errors())) << "\n";
        
        auto error_list = get_errors();
        if (!error_list.empty()) {
            std::cout << "\n" << _("error_details") << "\n";
            for (const auto& error : error_list) {
                std::cout << "  " << error << "\n";
            }
        }
    }
};
