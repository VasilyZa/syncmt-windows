#pragma once

#include <windows.h>
#include <stdexcept>
#include <string>

class FileHandle {
    HANDLE handle;
public:
    explicit FileHandle(HANDLE h = INVALID_HANDLE_VALUE) : handle(h) {}
    ~FileHandle() {
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    FileHandle(FileHandle&& other) noexcept : handle(other.handle) {
        other.handle = INVALID_HANDLE_VALUE;
    }
    FileHandle& operator=(FileHandle&& other) noexcept {
        if (this != &other) {
            if (handle != INVALID_HANDLE_VALUE) {
                CloseHandle(handle);
            }
            handle = other.handle;
            other.handle = INVALID_HANDLE_VALUE;
        }
        return *this;
    }
    [[nodiscard]] HANDLE get() const { return handle; }
    void reset(HANDLE h = INVALID_HANDLE_VALUE) {
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
        handle = h;
    }
    HANDLE release() {
        HANDLE h = handle;
        handle = INVALID_HANDLE_VALUE;
        return h;
    }
};

class MappedFile {
    void* data;
    size_t size;
public:
    MappedFile() : data(nullptr), size(0) {}
    MappedFile(HANDLE h, size_t offset, size_t length) : size(length) {
        DWORD offset_high = static_cast<DWORD>(offset >> 32);
        DWORD offset_low = static_cast<DWORD>(offset & 0xFFFFFFFF);
        data = MapViewOfFile(h, FILE_MAP_READ, offset_high, offset_low, length);
        if (data == nullptr) {
            throw std::runtime_error("Failed to map view of file");
        }
    }
    ~MappedFile() {
        if (data != nullptr) {
            UnmapViewOfFile(data);
        }
    }
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    MappedFile(MappedFile&& other) noexcept : data(other.data), size(other.size) {
        other.data = nullptr;
        other.size = 0;
    }
    MappedFile& operator=(MappedFile&& other) noexcept {
        if (this != &other) {
            if (data != nullptr) {
                UnmapViewOfFile(data);
            }
            data = other.data;
            size = other.size;
            other.data = nullptr;
            other.size = 0;
        }
        return *this;
    }
    [[nodiscard]] void* get() const { return data; }
    [[nodiscard]] size_t get_size() const { return size; }
};

class FindHandle {
    HANDLE handle;
public:
    explicit FindHandle(HANDLE h) : handle(h) {}
    ~FindHandle() {
        if (handle != INVALID_HANDLE_VALUE) {
            FindClose(handle);
        }
    }
    FindHandle(const FindHandle&) = delete;
    FindHandle& operator=(const FindHandle&) = delete;
    FindHandle(FindHandle&& other) noexcept : handle(other.handle) {
        other.handle = INVALID_HANDLE_VALUE;
    }
    FindHandle& operator=(FindHandle&& other) noexcept {
        if (this != &other) {
            if (handle != INVALID_HANDLE_VALUE) {
                FindClose(handle);
            }
            handle = other.handle;
            other.handle = INVALID_HANDLE_VALUE;
        }
        return *this;
    }
    [[nodiscard]] HANDLE get() const { return handle; }
};

class EventHandle {
    HANDLE handle;
public:
    EventHandle() : handle(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        if (handle == nullptr) {
            throw std::runtime_error("Failed to create event");
        }
    }
    ~EventHandle() {
        if (handle != nullptr) {
            CloseHandle(handle);
        }
    }
    EventHandle(const EventHandle&) = delete;
    EventHandle& operator=(const EventHandle&) = delete;
    EventHandle(EventHandle&& other) noexcept : handle(other.handle) {
        other.handle = nullptr;
    }
    EventHandle& operator=(EventHandle&& other) noexcept {
        if (this != &other) {
            if (handle != nullptr) {
                CloseHandle(handle);
            }
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }
    [[nodiscard]] HANDLE get() const { return handle; }
    void reset() {
        ResetEvent(handle);
    }
    void set() {
        SetEvent(handle);
    }
};
