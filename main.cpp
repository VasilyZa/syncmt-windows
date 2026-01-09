#include <iostream>
#include <filesystem>
#include <vector>
#include <thread>
#include <mutex>
#include <queue>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <iomanip>
#include <windows.h>
#include <io.h>
#include <unordered_set>
#include <string>
#include <memory>
#include <new>
#include <winioctl.h>
#include <intrin.h>

namespace fs = std::filesystem;

#ifndef VERSION
#define VERSION "1.1.3"
#endif

#ifndef GIT_COMMIT
#define GIT_COMMIT "unknown"
#endif

#ifndef BUILD_DATE
#define BUILD_DATE "unknown"
#endif

constexpr size_t DEFAULT_THREADS = 4;
constexpr size_t MIN_CHUNK_SIZE = 64 * 1024;
constexpr size_t MAX_CHUNK_SIZE = 64 * 1024 * 1024;
constexpr size_t SMALL_FILE_THRESHOLD = 1024 * 1024;
constexpr size_t LARGE_FILE_THRESHOLD = 100 * 1024 * 1024;
constexpr size_t MMAP_THRESHOLD = 50 * 1024 * 1024;
constexpr size_t PROGRESS_UPDATE_INTERVAL_MS = 100;
constexpr size_t SSD_CHUNK_SIZE = 64 * 1024 * 1024;
constexpr size_t HDD_CHUNK_SIZE = 8 * 1024 * 1024;
constexpr size_t SSD_MAX_THREADS = 0;
constexpr size_t HDD_MAX_THREADS = 4;

enum class ConflictResolution {
    OVERWRITE,
    SKIP,
    FAIL
};

enum class DiskType {
    SSD,
    HDD,
    UNKNOWN
};

class PerformanceMonitor {
    std::atomic<uint64_t> read_bytes{0};
    std::atomic<uint64_t> write_bytes{0};
    std::atomic<uint64_t> read_time_ns{0};
    std::atomic<uint64_t> write_time_ns{0};
    std::atomic<uint64_t> peak_speed{0};
    std::chrono::high_resolution_clock::time_point start_time;
    std::mutex stats_mutex;

public:
    PerformanceMonitor() : start_time(std::chrono::high_resolution_clock::now()) {}

    void record_read(uint64_t bytes, uint64_t duration_ns) {
        read_bytes.fetch_add(bytes, std::memory_order_relaxed);
        read_time_ns.fetch_add(duration_ns, std::memory_order_relaxed);
    }

    void record_write(uint64_t bytes, uint64_t duration_ns) {
        write_bytes.fetch_add(bytes, std::memory_order_relaxed);
        write_time_ns.fetch_add(duration_ns, std::memory_order_relaxed);
    }

    void update_peak_speed(uint64_t bytes_per_sec) {
        uint64_t current_peak = peak_speed.load(std::memory_order_relaxed);
        while (bytes_per_sec > current_peak) {
            if (peak_speed.compare_exchange_weak(current_peak, bytes_per_sec, 
                std::memory_order_relaxed)) {
                break;
            }
        }
    }

    uint64_t get_peak_speed() const {
        return peak_speed.load(std::memory_order_relaxed);
    }

    double get_average_read_speed_mbps() const {
        uint64_t total_read = read_bytes.load(std::memory_order_relaxed);
        uint64_t total_time = read_time_ns.load(std::memory_order_relaxed);
        if (total_time == 0) return 0.0;
        return (total_read * 1e3) / total_time;
    }

    double get_average_write_speed_mbps() const {
        uint64_t total_write = write_bytes.load(std::memory_order_relaxed);
        uint64_t total_time = write_time_ns.load(std::memory_order_relaxed);
        if (total_time == 0) return 0.0;
        return (total_write * 1e3) / total_time;
    }

    uint64_t get_elapsed_ms() const {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();
    }
};

class DiskTypeDetector {
public:
    static DiskType detect_disk_type(const fs::path& path) {
        std::wstring root = path.root_path().wstring();
        if (root.empty()) {
            return DiskType::UNKNOWN;
        }

        std::wstring volume_path = L"\\\\.\\" + root.substr(0, root.size() - 1);
        HANDLE hVolume = CreateFileW(volume_path.c_str(), GENERIC_READ, 
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, 
                                     nullptr, OPEN_EXISTING, 0, nullptr);
        
        if (hVolume == INVALID_HANDLE_VALUE) {
            return DiskType::UNKNOWN;
        }

        STORAGE_PROPERTY_QUERY query{};
        query.PropertyId = StorageDeviceSeekPenaltyProperty;
        query.QueryType = PropertyStandardQuery;

        DEVICE_SEEK_PENALTY_DESCRIPTOR result{};
        DWORD bytes_returned = 0;

        BOOL success = DeviceIoControl(hVolume, IOCTL_STORAGE_QUERY_PROPERTY,
                                       &query, sizeof(query), &result, sizeof(result),
                                       &bytes_returned, nullptr);
        
        CloseHandle(hVolume);

        if (success && bytes_returned >= sizeof(result)) {
            return result.IncursSeekPenalty ? DiskType::HDD : DiskType::SSD;
        }

        return DiskType::UNKNOWN;
    }

    static bool is_ssd(DiskType type) {
        return type == DiskType::SSD;
    }

    static size_t get_optimal_chunk_size(uint64_t file_size, DiskType disk_type) {
        if (is_ssd(disk_type)) {
            return std::min(file_size, static_cast<uint64_t>(SSD_CHUNK_SIZE));
        } else {
            return std::min(file_size, static_cast<uint64_t>(HDD_CHUNK_SIZE));
        }
    }

    static size_t get_optimal_thread_count(DiskType disk_type) {
        size_t cpu_cores = std::thread::hardware_concurrency();
        if (cpu_cores == 0) cpu_cores = 4;

        if (is_ssd(disk_type)) {
            if (SSD_MAX_THREADS == 0) {
                return cpu_cores * 2;
            }
            return std::min(cpu_cores * 2, SSD_MAX_THREADS);
        } else {
            return std::min(cpu_cores, HDD_MAX_THREADS);
        }
    }
};

struct FileTask {
    fs::path src;
    fs::path dst;
    bool is_move;
    ConflictResolution conflict_resolution;
    PerformanceMonitor* perf_monitor;
    DiskType src_disk_type;
    DiskType dst_disk_type;
};

class FileHandle {
    HANDLE handle;
public:
    FileHandle(LPCWSTR path, DWORD access, DWORD share_mode, DWORD creation, DWORD flags = FILE_ATTRIBUTE_NORMAL) 
        : handle(CreateFileW(path, access, share_mode, nullptr, creation, flags, nullptr)) {
        if (handle == INVALID_HANDLE_VALUE) {
            DWORD error = GetLastError();
            std::wstring wide_path(path);
            std::string narrow_path(wide_path.begin(), wide_path.end());
            throw std::runtime_error(std::string("Failed to open file: ") + narrow_path + 
                                   " (error: " + std::to_string(error) + ")");
        }
    }

    FileHandle(LPCSTR path, DWORD access, DWORD share_mode, DWORD creation, DWORD flags = FILE_ATTRIBUTE_NORMAL) 
        : handle(CreateFileA(path, access, share_mode, nullptr, creation, flags, nullptr)) {
        if (handle == INVALID_HANDLE_VALUE) {
            DWORD error = GetLastError();
            throw std::runtime_error(std::string("Failed to open file: ") + path + 
                                   " (error: " + std::to_string(error) + ")");
        }
    }

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

    operator HANDLE() const { return handle; }
    HANDLE get() const { return handle; }
};

class MappedFile {
    void* addr;
    size_t length;
    HANDLE file_handle;
    HANDLE mapping_handle;

public:
    MappedFile(void* addr, size_t length, HANDLE file_handle, HANDLE mapping_handle) 
        : addr(addr), length(length), file_handle(file_handle), mapping_handle(mapping_handle) {}

    ~MappedFile() {
        if (addr != nullptr) {
            FlushViewOfFile(addr, length);
            UnmapViewOfFile(addr);
        }
        if (mapping_handle != nullptr) {
            CloseHandle(mapping_handle);
        }
        if (file_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(file_handle);
        }
    }

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    MappedFile(MappedFile&& other) noexcept 
        : addr(other.addr), length(other.length), 
          file_handle(other.file_handle), mapping_handle(other.mapping_handle) {
        other.addr = nullptr;
        other.length = 0;
        other.file_handle = INVALID_HANDLE_VALUE;
        other.mapping_handle = nullptr;
    }

    MappedFile& operator=(MappedFile&& other) noexcept {
        if (this != &other) {
            if (addr != nullptr) {
                FlushViewOfFile(addr, length);
                UnmapViewOfFile(addr);
            }
            if (mapping_handle != nullptr) {
                CloseHandle(mapping_handle);
            }
            if (file_handle != INVALID_HANDLE_VALUE) {
                CloseHandle(file_handle);
            }
            addr = other.addr;
            length = other.length;
            file_handle = other.file_handle;
            mapping_handle = other.mapping_handle;
            other.addr = nullptr;
            other.length = 0;
            other.file_handle = INVALID_HANDLE_VALUE;
            other.mapping_handle = nullptr;
        }
        return *this;
    }

    void* data() const { return addr; }
    size_t size() const { return length; }
};

class AsyncIOOperation {
    OVERLAPPED overlapped{};
    HANDLE event_handle;
    std::vector<uint8_t> buffer;
    bool is_read;

public:
    AsyncIOOperation(size_t buffer_size, uint64_t offset, bool read_op) 
        : is_read(read_op), buffer(buffer_size) {
        event_handle = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        if (event_handle == nullptr) {
            throw std::runtime_error("Failed to create event for async I/O");
        }
        overlapped.hEvent = event_handle;
        overlapped.Offset = static_cast<DWORD>(offset);
        overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
    }

    ~AsyncIOOperation() {
        if (event_handle != nullptr) {
            CloseHandle(event_handle);
        }
    }

    AsyncIOOperation(const AsyncIOOperation&) = delete;
    AsyncIOOperation& operator=(const AsyncIOOperation&) = delete;
    AsyncIOOperation(AsyncIOOperation&&) = delete;
    AsyncIOOperation& operator=(AsyncIOOperation&&) = delete;

    OVERLAPPED* get() { return &overlapped; }
    uint8_t* data() { return buffer.data(); }
    size_t size() const { return buffer.size(); }
    bool is_read_operation() const { return is_read; }

    void wait() {
        WaitForSingleObject(event_handle, INFINITE);
    }

    bool wait(DWORD timeout_ms) {
        return WaitForSingleObject(event_handle, timeout_ms) == WAIT_OBJECT_0;
    }
};

template<typename T>
class LockFreeQueue {
    struct Node {
        std::atomic<Node*> next;
        T data;

        Node(T&& item) : next(nullptr), data(std::move(item)) {}
    };

    std::atomic<Node*> head;
    std::atomic<Node*> tail;

public:
    LockFreeQueue() {
        Node* dummy = new Node(T());
        head.store(dummy, std::memory_order_relaxed);
        tail.store(dummy, std::memory_order_relaxed);
    }

    ~LockFreeQueue() {
        while (Node* head_node = head.load(std::memory_order_relaxed)) {
            head.store(head_node->next.load(std::memory_order_relaxed), std::memory_order_relaxed);
            delete head_node;
        }
    }

    void enqueue(T item) {
        Node* new_node = new Node(std::move(item));
        new_node->next.store(nullptr, std::memory_order_relaxed);

        Node* prev_tail = tail.exchange(new_node, std::memory_order_acq_rel);
        prev_tail->next.store(new_node, std::memory_order_release);
    }

    bool dequeue(T& result) {
        Node* old_head = head.load(std::memory_order_acquire);
        Node* old_tail = tail.load(std::memory_order_acquire);
        Node* next_node = old_head->next.load(std::memory_order_acquire);

        if (old_head == old_tail) {
            if (next_node == nullptr) {
                return false;
            }
            tail.compare_exchange_weak(old_tail, next_node, std::memory_order_acq_rel);
        } else {
            if (next_node == nullptr) {
                return false;
            }

            result = std::move(next_node->data);

            if (head.compare_exchange_weak(old_head, next_node, std::memory_order_acq_rel)) {
                delete old_head;
                return true;
            }
        }
        return false;
    }

    bool empty() {
        Node* old_head = head.load(std::memory_order_acquire);
        Node* old_tail = tail.load(std::memory_order_acquire);
        return old_head == old_tail && old_head->next.load(std::memory_order_acquire) == nullptr;
    }
};

class EnhancedMappedFile {
    HANDLE file_handle;
    HANDLE mapping_handle;
    uint8_t* base_addr;
    uint64_t file_size;
    uint64_t view_size;

    static constexpr uint64_t DEFAULT_VIEW_SIZE = 64 * 1024 * 1024;

public:
    EnhancedMappedFile(const fs::path& path, DWORD access, DWORD share_mode, 
                       DWORD creation, DWORD attributes = FILE_ATTRIBUTE_NORMAL)
        : file_handle(INVALID_HANDLE_VALUE), mapping_handle(nullptr), 
          base_addr(nullptr), file_size(0), view_size(DEFAULT_VIEW_SIZE) {
        
        std::wstring wpath = path.wstring();
        file_handle = CreateFileW(wpath.c_str(), access, share_mode, 
                                   nullptr, creation, attributes, nullptr);
        if (file_handle == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("Failed to open file: " + path.string());
        }

        LARGE_INTEGER size;
        if (!GetFileSizeEx(file_handle, &size)) {
            CloseHandle(file_handle);
            throw std::runtime_error("Failed to get file size");
        }
        file_size = static_cast<uint64_t>(size.QuadPart);

        if (file_size == 0) return;

        DWORD protect = (access & GENERIC_WRITE) ? PAGE_READWRITE : PAGE_READONLY;
        mapping_handle = CreateFileMappingW(file_handle, nullptr, protect, 
                                              0, 0, nullptr);
        if (mapping_handle == nullptr) {
            CloseHandle(file_handle);
            throw std::runtime_error("Failed to create file mapping");
        }

        DWORD desired_access = (access & GENERIC_WRITE) ? FILE_MAP_WRITE : FILE_MAP_READ;
        DWORD offset_high = 0;
        DWORD offset_low = 0;
        size_t view_size_to_map = static_cast<size_t>(std::min(file_size, view_size));

        base_addr = static_cast<uint8_t*>(MapViewOfFile(mapping_handle, desired_access, 
                                                          offset_high, offset_low, view_size_to_map));
        if (base_addr == nullptr) {
            CloseHandle(mapping_handle);
            CloseHandle(file_handle);
            throw std::runtime_error("Failed to map view of file");
        }
    }

    ~EnhancedMappedFile() {
        if (base_addr != nullptr) {
            FlushViewOfFile(base_addr, static_cast<SIZE_T>(view_size));
            UnmapViewOfFile(base_addr);
        }
        if (mapping_handle != nullptr) {
            CloseHandle(mapping_handle);
        }
        if (file_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(file_handle);
        }
    }

    EnhancedMappedFile(const EnhancedMappedFile&) = delete;
    EnhancedMappedFile& operator=(const EnhancedMappedFile&) = delete;
    EnhancedMappedFile(EnhancedMappedFile&&) = delete;
    EnhancedMappedFile& operator=(EnhancedMappedFile&&) = delete;

    uint8_t* data() const { return base_addr; }
    uint64_t size() const { return file_size; }

    void flush() {
        if (base_addr != nullptr) {
            FlushViewOfFile(base_addr, static_cast<SIZE_T>(view_size));
        }
    }
};

class ThreadPool {
public:
    ThreadPool(size_t num_threads, std::atomic<size_t>* processed_files_ptr, 
               std::atomic<uint64_t>* copied_bytes_ptr, ConflictResolution conflict_resolution, 
               bool verbose, std::unordered_set<fs::path>* created_dirs_ptr,
               PerformanceMonitor* perf_monitor)
        : stop(false), processed_files_ptr(processed_files_ptr), 
          copied_bytes_ptr(copied_bytes_ptr), conflict_resolution(conflict_resolution), 
          verbose(verbose), created_dirs_ptr(created_dirs_ptr), perf_monitor(perf_monitor) {
        workers.reserve(num_threads);
        for (size_t i = 0; i < num_threads; ++i) {
            workers.emplace_back([this] {
                for (;;) {
                    FileTask task;
                    if (!task_queue.dequeue(task)) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        if (stop.load(std::memory_order_acquire)) {
                            return;
                        }
                        continue;
                    }
                    process_task(task);
                }
            });
        }
    }

    ~ThreadPool() {
        stop.store(true, std::memory_order_release);
        for (std::thread &worker : workers)
            worker.join();
    }

    void enqueue(FileTask task) {
        task_queue.enqueue(std::move(task));
    }

private:
    std::vector<std::thread> workers;
    LockFreeQueue<FileTask> task_queue;
    std::atomic<bool> stop;
    std::atomic<size_t>* processed_files_ptr;
    std::atomic<uint64_t>* copied_bytes_ptr;
    ConflictResolution conflict_resolution;
    bool verbose;
    std::unordered_set<fs::path>* created_dirs_ptr;
    PerformanceMonitor* perf_monitor;

    [[nodiscard]] static size_t calculate_chunk_size(uint64_t file_size, DiskType disk_type) {
        if (file_size < SMALL_FILE_THRESHOLD) {
            return MIN_CHUNK_SIZE;
        } else if (file_size < LARGE_FILE_THRESHOLD) {
            size_t adaptive_size = file_size / 10;
            adaptive_size = std::max(adaptive_size, MIN_CHUNK_SIZE);
            adaptive_size = std::min(adaptive_size, MAX_CHUNK_SIZE);
            return adaptive_size;
        } else {
            size_t adaptive_size = file_size / 100;
            adaptive_size = std::max(adaptive_size, static_cast<size_t>(1024 * 1024));
            adaptive_size = std::min(adaptive_size, MAX_CHUNK_SIZE);
            return adaptive_size;
        }
    }

    [[nodiscard]] static size_t calculate_optimal_chunk_size(uint64_t file_size, DiskType disk_type) {
        if (file_size >= MMAP_THRESHOLD) {
            return DiskTypeDetector::get_optimal_chunk_size(file_size, disk_type);
        }
        return calculate_chunk_size(file_size, disk_type);
    }

    static void copy_file_mmap(const fs::path& src, const fs::path& dst, 
                               std::unordered_set<fs::path>& created_dirs,
                               PerformanceMonitor* perf_monitor) {
        try {
            EnhancedMappedFile src_mmap(src, GENERIC_READ, FILE_SHARE_READ, 
                                        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN);
            
            fs::path dst_parent = dst.parent_path();
            if (!dst_parent.empty()) {
                if (created_dirs.find(dst_parent) == created_dirs.end()) {
                    if (!fs::exists(dst_parent)) {
                        fs::create_directories(dst_parent);
                    }
                    created_dirs.insert(dst_parent);
                }
            }

            EnhancedMappedFile dst_mmap(dst, GENERIC_WRITE, 0, CREATE_ALWAYS, 
                                        FILE_ATTRIBUTE_NORMAL);
            
            uint64_t file_size = src_mmap.size();
            if (file_size == 0) {
                copy_file_attributes(src, dst);
                return;
            }

            auto start_time = std::chrono::high_resolution_clock::now();
            
            size_t chunk_size = 64 * 1024 * 1024;
            for (uint64_t offset = 0; offset < file_size; offset += chunk_size) {
                size_t bytes_to_copy = std::min(chunk_size, static_cast<size_t>(file_size - offset));
                
                memcpy(dst_mmap.data() + offset, src_mmap.data() + offset, bytes_to_copy);
            }
            
            dst_mmap.flush();

            auto end_time = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time);
            
            if (perf_monitor) {
                perf_monitor->record_read(file_size, duration.count());
                perf_monitor->record_write(file_size, duration.count());
                
                uint64_t elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
                if (elapsed_ms > 0) {
                    uint64_t speed = (file_size / elapsed_ms) * 1000;
                    perf_monitor->update_peak_speed(speed);
                }
            }

            copy_file_attributes(src, dst);
        } catch (const std::exception& e) {
            throw std::runtime_error(std::string("Memory mapped copy failed: ") + e.what() + 
                                   ", falling back to buffered copy");
        }
    }

    static void copy_file_async(const fs::path& src, const fs::path& dst, 
                                 std::unordered_set<fs::path>& created_dirs,
                                 PerformanceMonitor* perf_monitor, DiskType src_disk_type, DiskType dst_disk_type) {
        std::wstring src_path = src.wstring();
        std::wstring dst_path = dst.wstring();

        HANDLE src_handle = CreateFileW(src_path.c_str(), GENERIC_READ, FILE_SHARE_READ, 
                                         nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (src_handle == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("Failed to open source file for async I/O: " + src.string());
        }

        fs::path dst_parent = dst.parent_path();
        if (!dst_parent.empty()) {
            if (created_dirs.find(dst_parent) == created_dirs.end()) {
                if (!fs::exists(dst_parent)) {
                    fs::create_directories(dst_parent);
                }
                created_dirs.insert(dst_parent);
            }
        }

        HANDLE dst_handle = CreateFileW(dst_path.c_str(), GENERIC_WRITE, 0, 
                                         nullptr, CREATE_ALWAYS, FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (dst_handle == INVALID_HANDLE_VALUE) {
            CloseHandle(src_handle);
            throw std::runtime_error("Failed to open destination file for async I/O: " + dst.string());
        }

        LARGE_INTEGER file_size;
        if (!GetFileSizeEx(src_handle, &file_size)) {
            CloseHandle(dst_handle);
            CloseHandle(src_handle);
            throw std::runtime_error("Failed to get file size: " + src.string());
        }

        if (file_size.QuadPart == 0) {
            CloseHandle(dst_handle);
            CloseHandle(src_handle);
            return;
        }

        size_t chunk_size = calculate_optimal_chunk_size(file_size.QuadPart, src_disk_type);
        const size_t pipeline_depth = 2;

        std::vector<std::unique_ptr<AsyncIOOperation>> read_ops;
        std::vector<std::unique_ptr<AsyncIOOperation>> write_ops;

        for (size_t i = 0; i < pipeline_depth; ++i) {
            read_ops.emplace_back(std::make_unique<AsyncIOOperation>(chunk_size, 0, true));
            write_ops.emplace_back(std::make_unique<AsyncIOOperation>(chunk_size, 0, false));
        }

        uint64_t total_bytes_copied = 0;
        uint64_t read_offset = 0;
        uint64_t write_offset = 0;
        size_t read_index = 0;
        size_t write_index = 0;

        auto start_time = std::chrono::high_resolution_clock::now();

        for (size_t i = 0; i < pipeline_depth; ++i) {
            if (read_offset >= static_cast<uint64_t>(file_size.QuadPart)) break;
            
            DWORD bytes_to_read = static_cast<DWORD>(std::min(static_cast<uint64_t>(chunk_size), 
                                                             static_cast<uint64_t>(file_size.QuadPart - read_offset)));
            
            read_ops[read_index]->get()->Offset = static_cast<DWORD>(read_offset);
            read_ops[read_index]->get()->OffsetHigh = static_cast<DWORD>(read_offset >> 32);
            
            if (!ReadFile(src_handle, read_ops[read_index]->data(), bytes_to_read, 
                          nullptr, read_ops[read_index]->get())) {
                DWORD error = GetLastError();
                if (error != ERROR_IO_PENDING) {
                    CloseHandle(dst_handle);
                    CloseHandle(src_handle);
                    throw std::runtime_error("Async read failed: " + src.string());
                }
            }
            
            read_offset += bytes_to_read;
            read_index = (read_index + 1) % pipeline_depth;
        }

        while (write_offset < static_cast<uint64_t>(file_size.QuadPart)) {
            size_t pending_read_index = (read_index + pipeline_depth - 1) % pipeline_depth;
            
            read_ops[pending_read_index]->wait();
            DWORD bytes_read = 0;
            if (!GetOverlappedResult(src_handle, read_ops[pending_read_index]->get(), &bytes_read, FALSE)) {
                CloseHandle(dst_handle);
                CloseHandle(src_handle);
                throw std::runtime_error("Get overlapped result failed for read");
            }

            memcpy(write_ops[write_index]->data(), read_ops[pending_read_index]->data(), bytes_read);
            
            write_ops[write_index]->get()->Offset = static_cast<DWORD>(write_offset);
            write_ops[write_index]->get()->OffsetHigh = static_cast<DWORD>(write_offset >> 32);
            
            if (!WriteFile(dst_handle, write_ops[write_index]->data(), bytes_read, 
                           nullptr, write_ops[write_index]->get())) {
                DWORD error = GetLastError();
                if (error != ERROR_IO_PENDING) {
                    CloseHandle(dst_handle);
                    CloseHandle(src_handle);
                    throw std::runtime_error("Async write failed: " + dst.string());
                }
            }

            write_offset += bytes_read;
            total_bytes_copied += bytes_read;

            write_ops[write_index]->wait();
            DWORD bytes_written = 0;
            if (!GetOverlappedResult(dst_handle, write_ops[write_index]->get(), &bytes_written, FALSE)) {
                CloseHandle(dst_handle);
                CloseHandle(src_handle);
                throw std::runtime_error("Get overlapped result failed for write");
            }

            write_index = (write_index + 1) % pipeline_depth;

            if (read_offset < static_cast<uint64_t>(file_size.QuadPart)) {
                DWORD bytes_to_read = static_cast<DWORD>(std::min(static_cast<uint64_t>(chunk_size), 
                                                                 static_cast<uint64_t>(file_size.QuadPart - read_offset)));
                
                read_ops[read_index]->get()->Offset = static_cast<DWORD>(read_offset);
                read_ops[read_index]->get()->OffsetHigh = static_cast<DWORD>(read_offset >> 32);
                
                if (!ReadFile(src_handle, read_ops[read_index]->data(), bytes_to_read, 
                              nullptr, read_ops[read_index]->get())) {
                    DWORD error = GetLastError();
                    if (error != ERROR_IO_PENDING) {
                        CloseHandle(dst_handle);
                        CloseHandle(src_handle);
                        throw std::runtime_error("Async read failed: " + src.string());
                    }
                }
                
                read_offset += bytes_to_read;
                read_index = (read_index + 1) % pipeline_depth;
            }
        }

        FlushFileBuffers(dst_handle);
        CloseHandle(dst_handle);
        CloseHandle(src_handle);

        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time);
        
        if (perf_monitor) {
            perf_monitor->record_read(total_bytes_copied, duration.count());
            perf_monitor->record_write(total_bytes_copied, duration.count());
            
            uint64_t elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
            if (elapsed_ms > 0) {
                uint64_t speed = (total_bytes_copied / elapsed_ms) * 1000;
                perf_monitor->update_peak_speed(speed);
            }
        }

        copy_file_attributes(src, dst);
    }

    static void copy_file_attributes(const fs::path& src, const fs::path& dst) {
        FileHandle src_handle(src.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, 
                              OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
        
        BY_HANDLE_FILE_INFORMATION file_info;
        if (GetFileInformationByHandle(src_handle.get(), &file_info)) {
            FileHandle dst_handle(dst.wstring().c_str(), GENERIC_WRITE, 
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, 
                                  OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
            
            FILETIME creation_time = file_info.ftCreationTime;
            FILETIME access_time = file_info.ftLastAccessTime;
            FILETIME write_time = file_info.ftLastWriteTime;
            SetFileTime(dst_handle.get(), &creation_time, &access_time, &write_time);
        }
    }

    void process_task(const FileTask& task) {
        try {
            if (fs::exists(task.dst)) {
                switch (task.conflict_resolution) {
                    case ConflictResolution::SKIP:
                        if (verbose) {
                            std::lock_guard<std::mutex> lock(cout_mutex);
                            std::cout << "Skipping existing file: " << task.dst << std::endl;
                        }
                        (*processed_files_ptr)++;
                        return;
                    case ConflictResolution::OVERWRITE:
                        if (verbose) {
                            std::lock_guard<std::mutex> lock(cout_mutex);
                            std::cout << "Overwriting existing file: " << task.dst << std::endl;
                        }
                        break;
                    case ConflictResolution::FAIL:
                        throw std::runtime_error(std::string("Destination file already exists: ") + 
                                               task.dst.string());
                }
            }

            uint64_t file_size = fs::file_size(task.src);
            
            if (task.is_move) {
                move_file(task.src, task.dst, *created_dirs_ptr, task.perf_monitor, 
                         task.src_disk_type, task.dst_disk_type);
                (*processed_files_ptr)++;
                (*copied_bytes_ptr) += file_size;
                return;
            }

            if (file_size >= MMAP_THRESHOLD) {
                try {
                    copy_file_mmap(task.src, task.dst, *created_dirs_ptr, task.perf_monitor);
                    (*processed_files_ptr)++;
                    (*copied_bytes_ptr) += file_size;
                    return;
                } catch (const std::exception& e) {
                    if (verbose) {
                        std::cerr << "Memory mapped copy failed for " << task.src 
                                 << ", falling back to buffered copy: " << e.what() << std::endl;
                    }
                }
            }

            copy_file(task.src, task.dst, *created_dirs_ptr, task.perf_monitor, 
                     task.src_disk_type, task.dst_disk_type);
            (*processed_files_ptr)++;
            (*copied_bytes_ptr) += file_size;
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(cout_mutex);
            std::cerr << "Error processing " << task.src << ": " << e.what() << std::endl;
        }
    }

    static void copy_file(const fs::path& src, const fs::path& dst, std::unordered_set<fs::path>& created_dirs, 
                         PerformanceMonitor* perf_monitor, DiskType src_disk_type, DiskType dst_disk_type) {
        std::wstring src_path = src.wstring();
        std::wstring dst_path = dst.wstring();

        FileHandle src_handle(src_path.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN);
        
        LARGE_INTEGER file_size;
        if (!GetFileSizeEx(src_handle.get(), &file_size)) {
            DWORD error = GetLastError();
            throw std::runtime_error(std::string("Failed to get file size: ") + src.string() +
                                   " (error: " + std::to_string(error) + ")");
        }

        fs::path dst_parent = dst.parent_path();
        if (!dst_parent.empty()) {
            if (created_dirs.find(dst_parent) == created_dirs.end()) {
                if (!fs::exists(dst_parent)) {
                    fs::create_directories(dst_parent);
                }
                created_dirs.insert(dst_parent);
            }
        }

        FileHandle dst_handle(dst_path.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL);

        if (file_size.QuadPart > 0) {
            LARGE_INTEGER new_size;
            new_size.QuadPart = file_size.QuadPart;
            if (!SetFilePointerEx(dst_handle.get(), new_size, nullptr, FILE_BEGIN) ||
                !SetEndOfFile(dst_handle.get())) {
                DWORD error = GetLastError();
                if (error != ERROR_NOT_SUPPORTED) {
                    throw std::runtime_error(std::string("Failed to set file size: ") + dst.string() +
                                           " (error: " + std::to_string(error) + ")");
                }
            }
        }

        SetFilePointer(dst_handle.get(), 0, nullptr, FILE_BEGIN);

        size_t chunk_size = calculate_optimal_chunk_size(file_size.QuadPart, src_disk_type);
        std::vector<char> buffer;
        buffer.resize(chunk_size);

        LARGE_INTEGER remaining;
        remaining.QuadPart = file_size.QuadPart;
        
        auto start_time = std::chrono::high_resolution_clock::now();

        while (remaining.QuadPart > 0) {
            DWORD to_read = static_cast<DWORD>(std::min(static_cast<uint64_t>(buffer.size()), static_cast<uint64_t>(remaining.QuadPart)));
            DWORD bytes_read = 0;
            
            if (!ReadFile(src_handle.get(), buffer.data(), to_read, &bytes_read, nullptr)) {
                DWORD error = GetLastError();
                throw std::runtime_error(std::string("Failed to read file: ") + src.string() + 
                                       " (error: " + std::to_string(error) + ")");
            }

            if (bytes_read == 0) {
                break;
            }

            DWORD bytes_written = 0;
            if (!WriteFile(dst_handle.get(), buffer.data(), bytes_read, &bytes_written, nullptr)) {
                DWORD error = GetLastError();
                throw std::runtime_error(std::string("Failed to write file: ") + dst.string() +
                                       " (error: " + std::to_string(error) + ")");
            }

            if (bytes_written != bytes_read) {
                throw std::runtime_error(std::string("Write incomplete: ") + dst.string());
            }

            remaining.QuadPart -= bytes_written;
            
            _mm_prefetch(reinterpret_cast<const char*>(buffer.data()), _MM_HINT_T0);
        }
        
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time);
        
        if (perf_monitor) {
            perf_monitor->record_read(file_size.QuadPart, duration.count());
            perf_monitor->record_write(file_size.QuadPart, duration.count());
            
            uint64_t elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
            if (elapsed_ms > 0) {
                uint64_t speed = (file_size.QuadPart / elapsed_ms) * 1000;
                perf_monitor->update_peak_speed(speed);
            }
        }

        BY_HANDLE_FILE_INFORMATION file_info;
        if (GetFileInformationByHandle(src_handle.get(), &file_info)) {
            FILETIME creation_time = file_info.ftCreationTime;
            FILETIME access_time = file_info.ftLastAccessTime;
            FILETIME write_time = file_info.ftLastWriteTime;
            SetFileTime(dst_handle.get(), &creation_time, &access_time, &write_time);
        }
    }

    static void move_file(const fs::path& src, const fs::path& dst, std::unordered_set<fs::path>& created_dirs,
                         PerformanceMonitor* perf_monitor, DiskType src_disk_type, DiskType dst_disk_type) {
        fs::path dst_parent = dst.parent_path();
        if (!dst_parent.empty()) {
            if (created_dirs.find(dst_parent) == created_dirs.end()) {
                if (!fs::exists(dst_parent)) {
                    fs::create_directories(dst_parent);
                }
                created_dirs.insert(dst_parent);
            }
        }

        std::wstring src_path = src.wstring();
        std::wstring dst_path = dst.wstring();

        if (MoveFileExW(src_path.c_str(), dst_path.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING)) {
            return;
        }

        DWORD error = GetLastError();
        if (error == ERROR_NOT_SAME_DEVICE) {
            copy_file(src, dst, created_dirs, perf_monitor, src_disk_type, dst_disk_type);
            fs::remove(src);
        } else {
            throw std::runtime_error(std::string("Failed to move file: ") + src.string() +
                                   " (error: " + std::to_string(error) + ")");
        }
    }

    static std::mutex cout_mutex;
};

std::mutex ThreadPool::cout_mutex;

class FileCopier {
public:
    FileCopier(size_t num_threads, bool move_mode, bool verbose, ConflictResolution conflict_resolution)
        : pool(num_threads, &processed_files, &copied_bytes, conflict_resolution, verbose, &created_dirs, &perf_monitor), 
          move_mode(move_mode), verbose(verbose), 
          conflict_resolution(conflict_resolution),
          total_files(0), processed_files(0), total_bytes(0), copied_bytes(0),
          start_time(std::chrono::high_resolution_clock::now()), 
          last_progress_update(start_time) {}

    void process(const std::vector<fs::path>& sources, const fs::path& dst) {
        start_time = std::chrono::high_resolution_clock::now();
        last_progress_update = start_time;

        for (const auto& src : sources) {
            if (fs::is_directory(src)) {
                process_directory(src, dst / src.filename());
            } else if (fs::is_regular_file(src)) {
                fs::path actual_dst = dst;
                if (fs::exists(dst) && fs::is_directory(dst)) {
                    actual_dst = dst / src.filename();
                }
                process_single_file(src, actual_dst);
            } else {
                std::cerr << "Warning: Skipping non-regular file: " << src << std::endl;
            }
        }

        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

        std::cout << "\r" << std::string(100, ' ') << "\r";
        print_stats(duration);
    }

    void process(const fs::path& src, const fs::path& dst) {
        start_time = std::chrono::high_resolution_clock::now();
        last_progress_update = start_time;

        if (fs::is_directory(src)) {
            process_directory(src, dst);
        } else {
            fs::path actual_dst = dst;
            if (fs::exists(dst) && fs::is_directory(dst)) {
                actual_dst = dst / src.filename();
            }
            process_single_file(src, actual_dst);
        }

        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

        std::cout << "\r" << std::string(100, ' ') << "\r";
        print_stats(duration);
    }

private:
    ThreadPool pool;
    bool move_mode;
    bool verbose;
    ConflictResolution conflict_resolution;
    std::atomic<size_t> total_files;
    std::atomic<size_t> processed_files;
    std::atomic<uint64_t> total_bytes;
    std::atomic<uint64_t> copied_bytes;
    std::chrono::high_resolution_clock::time_point start_time;
    std::chrono::high_resolution_clock::time_point last_progress_update;
    std::unordered_set<fs::path> created_dirs;
    std::mutex cout_mutex;
    PerformanceMonitor perf_monitor;

    void scan_directory_native(const fs::path& src_dir, const fs::path& dst_dir, 
                                std::vector<FileTask>& tasks, size_t& scan_counter, bool pipeline = false) {
        std::wstring search_pattern = src_dir.wstring() + L"\\*";
        WIN32_FIND_DATAW find_data;
        HANDLE find_handle = FindFirstFileW(search_pattern.c_str(), &find_data);

        if (find_handle == INVALID_HANDLE_VALUE) {
            DWORD error = GetLastError();
            throw std::runtime_error(std::string("Failed to open directory: ") + src_dir.string() +
                                   " (error: " + std::to_string(error) + ")");
        }

        do {
            std::wstring filename = find_data.cFileName;
            if (filename == L"." || filename == L"..") {
                continue;
            }

            fs::path entry_path = src_dir / filename;

            if ((find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                fs::path rel_path = fs::relative(entry_path, src_dir);
                fs::path dst_path = dst_dir / rel_path;

                LARGE_INTEGER file_size;
                file_size.HighPart = find_data.nFileSizeHigh;
                file_size.LowPart = find_data.nFileSizeLow;

                total_files++;
                total_bytes += file_size.QuadPart;

                FileTask task{entry_path, dst_path, move_mode, conflict_resolution, &perf_monitor, 
                             DiskTypeDetector::detect_disk_type(entry_path),
                             DiskTypeDetector::detect_disk_type(dst_path)};
                
                if (pipeline) {
                    pool.enqueue(task);
                } else {
                    tasks.push_back(task);
                }

                scan_counter++;
                if (scan_counter % 1000 == 0) {
                    update_scan_progress(total_files.load(), total_bytes.load());
                    
                    if (!pipeline && tasks.size() >= 1000) {
                        for (auto& t : tasks) {
                            pool.enqueue(std::move(t));
                        }
                        tasks.clear();
                    }
                }
            } else if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                scan_directory_native(entry_path, dst_dir / filename, tasks, scan_counter, pipeline);
            }
        } while (FindNextFileW(find_handle, &find_data));

        DWORD error = GetLastError();
        FindClose(find_handle);
        
        if (error != ERROR_NO_MORE_FILES) {
            throw std::runtime_error(std::string("Error scanning directory: ") + src_dir.string() +
                                   " (error: " + std::to_string(error) + ")");
        }
    }

    void update_progress() {
        auto now = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_progress_update);

        if (elapsed.count() < PROGRESS_UPDATE_INTERVAL_MS) {
            return;
        }

        last_progress_update = now;

        size_t processed = processed_files.load();
        size_t total = total_files.load();
        uint64_t copied = copied_bytes.load();
        uint64_t total_b = total_bytes.load();

        double file_progress = total > 0 ? (processed * 100.0 / total) : 0.0;
        double byte_progress = total_b > 0 ? (copied * 100.0 / total_b) : 0.0;

        auto total_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time);
        double seconds = total_elapsed.count() / 1000.0;
        double speed = copied / (1024.0 * 1024.0) / seconds;

        double remaining_bytes = total_b - copied;
        double eta = speed > 0 ? remaining_bytes / (1024.0 * 1024.0) / speed : 0;

        int bar_width = 40;
        int filled = static_cast<int>(file_progress * bar_width / 100.0);

        std::lock_guard<std::mutex> lock(cout_mutex);
        std::cout << "\r[";
        for (int i = 0; i < bar_width; ++i) {
            if (i < filled) {
                std::cout << "=";
            } else if (i == filled) {
                std::cout << ">";
            } else {
                std::cout << " ";
            }
        }
        std::cout << "] ";
        std::cout << std::fixed << std::setprecision(1) << file_progress << "% ";
        std::cout << "(" << processed << "/" << total << " files, ";
        std::cout << std::setprecision(2) << (copied / (1024.0 * 1024.0)) << "/";
        std::cout << std::setprecision(2) << (total_b / (1024.0 * 1024.0)) << " MB, ";
        std::cout << std::setprecision(1) << speed << " MB/s, ETA: ";
        
        if (eta < 60) {
            std::cout << std::setprecision(0) << eta << "s";
        } else if (eta < 3600) {
            std::cout << std::setprecision(0) << (eta / 60) << "m" 
                      << std::setprecision(0) << (static_cast<int>(eta) % 60) << "s";
        } else {
            int hours = static_cast<int>(eta) / 3600;
            int minutes = (static_cast<int>(eta) % 3600) / 60;
            std::cout << hours << "h" << minutes << "m";
        }
        
        std::cout << ")";
        std::cout.flush();
    }

    void update_scan_progress(size_t scanned_files, uint64_t scanned_bytes) {
        std::cout << "\rScanning: [" << scanned_files << " files, " 
                  << std::fixed << std::setprecision(2) 
                  << (scanned_bytes / (1024.0 * 1024.0)) << " MB]... ";
        std::cout.flush();
    }

    void process_directory(const fs::path& src_dir, const fs::path& dst_dir) {
        std::cout << "Scanning directory: " << src_dir << "..." << std::endl;
        std::cout.flush();

        std::atomic<bool> scan_done{false};
        std::thread scan_thread([this, &src_dir, &dst_dir, &scan_done]() {
            std::vector<FileTask> tasks;
            size_t scan_counter = 0;
            scan_directory_native(src_dir, dst_dir, tasks, scan_counter, false);
            
            for (auto& task : tasks) {
                pool.enqueue(std::move(task));
            }
            
            scan_done.store(true);
        });

        auto scan_start_time = std::chrono::high_resolution_clock::now();

        while (!scan_done.load() || processed_files.load() < total_files.load()) {
            if (!scan_done.load()) {
                update_scan_progress(total_files.load(), total_bytes.load());
            } else {
                update_progress();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(PROGRESS_UPDATE_INTERVAL_MS));
        }

        scan_thread.join();

        auto scan_end_time = std::chrono::high_resolution_clock::now();
        auto scan_duration = std::chrono::duration_cast<std::chrono::milliseconds>(scan_end_time - scan_start_time);
        double scan_seconds = scan_duration.count() / 1000.0;

        std::cout << "\r" << std::string(100, ' ') << "\r";
        std::cout << "Scan complete: [" << total_files.load() << " files, " 
                  << std::fixed << std::setprecision(2) 
                  << (total_bytes.load() / (1024.0 * 1024.0)) << " MB] ("
                  << std::setprecision(2) << scan_seconds << "s)" << std::endl;
    }

    void process_single_file(const fs::path& src, const fs::path& dst) {
        total_files = 1;
        total_bytes = fs::file_size(src);

        std::cout << "Processing single file: " << src << "..." << std::endl;
        std::cout.flush();

        FileTask task{src, dst, move_mode, conflict_resolution, &perf_monitor, 
                     DiskTypeDetector::detect_disk_type(src),
                     DiskTypeDetector::detect_disk_type(dst)};
        pool.enqueue(task);

        auto timeout = std::chrono::high_resolution_clock::now() + std::chrono::seconds(30);
        while (processed_files.load() < total_files.load()) {
            if (std::chrono::high_resolution_clock::now() > timeout) {
                throw std::runtime_error("Timeout processing file: " + src.string());
            }
            update_progress();
            std::this_thread::sleep_for(std::chrono::milliseconds(PROGRESS_UPDATE_INTERVAL_MS));
        }

        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        print_stats(duration);
    }

    void print_stats(std::chrono::milliseconds duration) {
        double seconds = duration.count() / 1000.0;
        uint64_t copied = copied_bytes.load();
        double speed = copied / (1024.0 * 1024.0) / seconds;

        std::cout << "Files processed: " << processed_files.load() << std::endl;
        std::cout << "Total copied: " << std::fixed << std::setprecision(2) 
                  << (copied / (1024.0 * 1024.0)) << " MB" << std::endl;
        std::cout << "Average speed: " << std::setprecision(1) << speed << " MB/s" << std::endl;
        
        double avg_read_speed = perf_monitor.get_average_read_speed_mbps();
        double avg_write_speed = perf_monitor.get_average_write_speed_mbps();
        uint64_t peak_speed = perf_monitor.get_peak_speed();
        
        if (avg_read_speed > 0 || avg_write_speed > 0) {
            std::cout << "Average read speed: " << std::setprecision(1) << avg_read_speed << " MB/s" << std::endl;
            std::cout << "Average write speed: " << std::setprecision(1) << avg_write_speed << " MB/s" << std::endl;
        }
        
        if (peak_speed > 0) {
            std::cout << "Peak speed: " << std::fixed << std::setprecision(2) 
                      << (peak_speed / (1024.0 * 1024.0)) << " MB/s" << std::endl;
        }
        
        std::cout << "Total time: " << std::setprecision(2) << seconds << "s" << std::endl;
    }
};

void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [OPTIONS] <source>... <destination>" << std::endl;
    std::cout << "\nOptions:" << std::endl;
    std::cout << "  -t, --threads <num>   Number of threads (default: " << DEFAULT_THREADS << ", use 'auto' for auto-detection)" << std::endl;
    std::cout << "  -m, --move            Move files instead of copying" << std::endl;
    std::cout << "  -v, --verbose         Enable verbose output" << std::endl;
    std::cout << "  -o, --overwrite       Overwrite existing files" << std::endl;
    std::cout << "  -s, --skip            Skip existing files" << std::endl;
    std::cout << "  -V, --version         Print version information" << std::endl;
    std::cout << "  -h, --help            Show this help message" << std::endl;
}

void print_version() {
    std::cout << "syncmt version " << VERSION << std::endl;
    std::cout << "Git commit: " << GIT_COMMIT << std::endl;
    std::cout << "Build date: " << BUILD_DATE << std::endl;
}

int main(int argc, char* argv[]) {
    size_t num_threads = DEFAULT_THREADS;
    bool move_mode = false;
    bool verbose = false;
    bool auto_threads = false;
    ConflictResolution conflict_resolution = ConflictResolution::FAIL;
    std::vector<std::string> src_paths;
    std::string dst_path;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-t" || arg == "--threads") {
            if (i + 1 < argc) {
                std::string thread_arg = argv[++i];
                if (thread_arg == "auto") {
                    auto_threads = true;
                } else {
                    num_threads = std::stoul(thread_arg);
                }
            }
        } else if (arg == "-m" || arg == "--move") {
            move_mode = true;
        } else if (arg == "-v" || arg == "--verbose") {
            verbose = true;
        } else if (arg == "-o" || arg == "--overwrite") {
            conflict_resolution = ConflictResolution::OVERWRITE;
        } else if (arg == "-s" || arg == "--skip") {
            conflict_resolution = ConflictResolution::SKIP;
        } else if (arg == "-V" || arg == "--version") {
            print_version();
            return 0;
        } else if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else {
            src_paths.push_back(arg);
        }
    }

    if (src_paths.size() < 2) {
        print_usage(argv[0]);
        return 1;
    }

    dst_path = src_paths.back();
    src_paths.pop_back();

    for (const auto& src : src_paths) {
        if (!fs::exists(src)) {
            std::cerr << "Error: Source path does not exist: " << src << std::endl;
            return 1;
        }
    }

    if (auto_threads) {
        DiskType src_disk_type = DiskTypeDetector::detect_disk_type(src_paths[0]);
        DiskType dst_disk_type = DiskTypeDetector::detect_disk_type(dst_path);
        
        size_t src_threads = DiskTypeDetector::get_optimal_thread_count(src_disk_type);
        size_t dst_threads = DiskTypeDetector::get_optimal_thread_count(dst_disk_type);
        num_threads = std::min(src_threads, dst_threads);
        
        if (verbose) {
            std::cout << "Auto-detected thread count: " << num_threads << std::endl;
        }
    }

    try {
        std::cout << "Starting " << (move_mode ? "move" : "copy") << " operation..." << std::endl;
        std::cout << "Source(s): ";
        for (size_t i = 0; i < src_paths.size(); ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << src_paths[i];
        }
        std::cout << std::endl;
        std::cout << "Destination: " << dst_path << std::endl;
        std::cout << "Threads: " << num_threads << std::endl;
        std::cout << "Conflict resolution: ";
        switch (conflict_resolution) {
            case ConflictResolution::OVERWRITE:
                std::cout << "overwrite" << std::endl;
                break;
            case ConflictResolution::SKIP:
                std::cout << "skip" << std::endl;
                break;
            case ConflictResolution::FAIL:
                std::cout << "error on conflict" << std::endl;
                break;
        }

        FileCopier copier(num_threads, move_mode, verbose, conflict_resolution);

        std::vector<fs::path> sources;
        for (const auto& src : src_paths) {
            sources.push_back(src);
        }

        copier.process(sources, dst_path);

        std::cout << "\nOperation completed successfully!" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}
