#pragma once

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <atomic>
#include <filesystem>
#include <unordered_set>
#include <windows.h>
#include <system_error>
#include "file_handle.h"
#include "file_operations.h"

namespace fs = std::filesystem;

class ThreadPool {
private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    std::atomic<bool> stop;
    
public:
    explicit ThreadPool(size_t num_threads = std::thread::hardware_concurrency()) : stop(false) {
        for (size_t i = 0; i < num_threads; ++i) {
            workers.emplace_back([this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(this->queue_mutex);
                        this->condition.wait(lock, [this] {
                            return this->stop || !this->tasks.empty();
                        });
                        if (this->stop && this->tasks.empty()) {
                            return;
                        }
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                    }
                    task();
                }
            });
        }
    }
    
    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            stop = true;
        }
        condition.notify_all();
        for (auto& worker : workers) {
            worker.join();
        }
    }
    
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    
    template<class F>
    void enqueue(F&& f) {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            tasks.emplace(std::forward<F>(f));
        }
        condition.notify_one();
    }
    
    void wait_for_tasks() {
        std::unique_lock<std::mutex> lock(queue_mutex);
        condition.wait(lock, [this] {
            return this->tasks.empty();
        });
    }
    
    static void copy_file_mmap(const fs::path& src, const fs::path& dst, std::unordered_set<fs::path>& created_dirs) {
        ensure_directory_exists(dst.parent_path(), created_dirs);
        
        FileHandle src_handle(CreateFileW(src.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (src_handle.get() == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("Failed to open source file: " + src.string());
        }
        
        uintmax_t file_size = get_file_size(src);
        if (file_size == 0) {
            FileHandle dst_handle(CreateFileW(dst.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (dst_handle.get() == INVALID_HANDLE_VALUE) {
                throw std::runtime_error("Failed to create destination file: " + dst.string());
            }
            return;
        }
        
        if (!is_disk_space_available(dst.parent_path(), file_size)) {
            throw std::runtime_error("Insufficient disk space for: " + dst.string());
        }
        
        FileHandle mapping_handle(CreateFileMappingW(src_handle.get(), nullptr, PAGE_READONLY, 0, 0, nullptr));
        if (mapping_handle.get() == nullptr) {
            throw std::runtime_error("Failed to create file mapping: " + src.string());
        }
        
        FileHandle dst_handle(CreateFileW(dst.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (dst_handle.get() == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("Failed to create destination file: " + dst.string());
        }
        
        FileHandle dst_mapping(CreateFileMappingW(dst_handle.get(), nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(file_size), nullptr));
        if (dst_mapping.get() == nullptr) {
            throw std::runtime_error("Failed to create destination file mapping: " + dst.string());
        }
        
        SYSTEM_INFO sys_info;
        GetSystemInfo(&sys_info);
        size_t granularity = sys_info.dwAllocationGranularity;
        size_t chunk_size = 64 * 1024 * 1024;
        chunk_size = (chunk_size / granularity) * granularity;
        
        for (size_t offset = 0; offset < file_size; offset += chunk_size) {
            size_t size_to_copy = std::min(chunk_size, file_size - offset);
            MappedFile src_view(mapping_handle.get(), offset, size_to_copy);
            MappedFile dst_view(dst_mapping.get(), offset, size_to_copy);
            
            memcpy(dst_view.get(), src_view.get(), size_to_copy);
        }
        
        copy_file_attributes(src, dst);
    }
    
    static void copy_file_async(const fs::path& src, const fs::path& dst, std::unordered_set<fs::path>& created_dirs) {
        ensure_directory_exists(dst.parent_path(), created_dirs);
        
        FileHandle src_handle(CreateFileW(src.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
        if (src_handle.get() == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("Failed to open source file: " + src.string());
        }
        
        uintmax_t file_size = get_file_size(src);
        if (file_size == 0) {
            FileHandle dst_handle(CreateFileW(dst.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (dst_handle.get() == INVALID_HANDLE_VALUE) {
                throw std::runtime_error("Failed to create destination file: " + dst.string());
            }
            return;
        }
        
        if (!is_disk_space_available(dst.parent_path(), file_size)) {
            throw std::runtime_error("Insufficient disk space for: " + dst.string());
        }
        
        FileHandle dst_handle(CreateFileW(dst.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_OVERLAPPED, nullptr));
        if (dst_handle.get() == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("Failed to create destination file: " + dst.string());
        }
        
        size_t block_size = get_optimal_block_size();
        std::vector<char> buffer(block_size);
        
        std::vector<EventHandle> events(2);
        std::vector<OVERLAPPED> overlapped(2);
        
        for (size_t i = 0; i < 2; ++i) {
            ZeroMemory(&overlapped[i], sizeof(OVERLAPPED));
            overlapped[i].hEvent = events[i].get();
        }
        
        std::atomic<bool> write_completed[2] = {false, false};
        std::atomic<bool> read_completed[2] = {false, false};
        
        auto async_read = [&](size_t block_index, size_t offset) {
            OVERLAPPED ov = {};
            ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFF);
            ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
            
            DWORD bytes_read = 0;
            if (!ReadFile(src_handle.get(), buffer.data(), static_cast<DWORD>(block_size), &bytes_read, &ov)) {
                DWORD error = GetLastError();
                if (error != ERROR_IO_PENDING) {
                    throw std::runtime_error("Failed to read file: " + src.string());
                }
                if (!GetOverlappedResult(src_handle.get(), &ov, &bytes_read, TRUE)) {
                    throw std::runtime_error("Failed to get overlapped result: " + src.string());
                }
            }
            read_completed[block_index] = true;
        };
        
        auto async_write = [&](size_t block_index, size_t offset, DWORD bytes_to_write) {
            OVERLAPPED ov = {};
            ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFF);
            ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
            
            DWORD bytes_written = 0;
            if (!WriteFile(dst_handle.get(), buffer.data(), bytes_to_write, &bytes_written, &ov)) {
                DWORD error = GetLastError();
                if (error != ERROR_IO_PENDING) {
                    throw std::runtime_error("Failed to write file: " + dst.string());
                }
                if (!GetOverlappedResult(dst_handle.get(), &ov, &bytes_written, TRUE)) {
                    throw std::runtime_error("Failed to get overlapped result: " + dst.string());
                }
            }
            write_completed[block_index] = true;
        };
        
        std::vector<std::thread> threads;
        size_t block_index = 0;
        
        for (size_t offset = 0; offset < file_size; offset += block_size) {
            size_t bytes_to_read = std::min(block_size, file_size - offset);
            
            threads.emplace_back([&, block_index, offset, bytes_to_read]() {
                async_read(block_index % 2, offset);
                async_write(block_index % 2, offset, static_cast<DWORD>(bytes_to_read));
            });
            
            block_index++;
        }
        
        for (auto& thread : threads) {
            thread.join();
        }
        
        copy_file_attributes(src, dst);
    }
};
