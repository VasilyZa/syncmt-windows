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

namespace fs = std::filesystem;

#ifndef VERSION
#define VERSION "1.1.3"
#endif

#ifndef GIT_COMMIT
#define GIT_COMMIT "unknown"
#endif

#ifndef BUILD_DATE
#define BUILD_DATE __DATE__
#endif

constexpr size_t DEFAULT_THREADS = 4;
constexpr size_t MIN_CHUNK_SIZE = 64 * 1024;
constexpr size_t MAX_CHUNK_SIZE = 16 * 1024 * 1024;
constexpr size_t SMALL_FILE_THRESHOLD = 1024 * 1024;
constexpr size_t LARGE_FILE_THRESHOLD = 100 * 1024 * 1024;
constexpr size_t PROGRESS_UPDATE_INTERVAL_MS = 100;

enum class ConflictResolution {
    OVERWRITE,
    SKIP,
    FAIL
};

struct FileTask {
    fs::path src;
    fs::path dst;
    bool is_move;
    ConflictResolution conflict_resolution;
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

class ThreadPool {
public:
    ThreadPool(size_t num_threads, std::atomic<size_t>* processed_files_ptr, 
               std::atomic<uint64_t>* copied_bytes_ptr, ConflictResolution conflict_resolution, 
               bool verbose, std::unordered_set<fs::path>* created_dirs_ptr)
        : stop(false), processed_files_ptr(processed_files_ptr), 
          copied_bytes_ptr(copied_bytes_ptr), conflict_resolution(conflict_resolution), 
          verbose(verbose), created_dirs_ptr(created_dirs_ptr) {
        workers.reserve(num_threads);
        for (size_t i = 0; i < num_threads; ++i) {
            workers.emplace_back([this] {
                for (;;) {
                    FileTask task;
                    {
                        std::unique_lock<std::mutex> lock(this->queue_mutex);
                        this->condition.wait(lock, [this] {
                            return this->stop || !this->tasks.empty();
                        });
                        if (this->stop && this->tasks.empty())
                            return;
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                    }
                    process_task(task);
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
        for (std::thread &worker : workers)
            worker.join();
    }

    void enqueue(FileTask task) {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            tasks.push(std::move(task));
        }
        condition.notify_one();
    }

private:
    std::vector<std::thread> workers;
    std::queue<FileTask> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;
    std::atomic<size_t>* processed_files_ptr;
    std::atomic<uint64_t>* copied_bytes_ptr;
    ConflictResolution conflict_resolution;
    bool verbose;
    std::unordered_set<fs::path>* created_dirs_ptr;

    [[nodiscard]] static size_t calculate_chunk_size(uint64_t file_size) {
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
                move_file(task.src, task.dst, *created_dirs_ptr);
            } else {
                copy_file(task.src, task.dst, *created_dirs_ptr);
            }
            (*processed_files_ptr)++;
            (*copied_bytes_ptr) += file_size;
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(cout_mutex);
            std::cerr << "Error processing " << task.src << ": " << e.what() << std::endl;
        }
    }

    static void copy_file(const fs::path& src, const fs::path& dst, std::unordered_set<fs::path>& created_dirs) {
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

        std::vector<char> buffer;
        buffer.resize(calculate_chunk_size(file_size.QuadPart));

        LARGE_INTEGER remaining;
        remaining.QuadPart = file_size.QuadPart;

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
        }

        BY_HANDLE_FILE_INFORMATION file_info;
        if (GetFileInformationByHandle(src_handle.get(), &file_info)) {
            FILETIME creation_time = file_info.ftCreationTime;
            FILETIME access_time = file_info.ftLastAccessTime;
            FILETIME write_time = file_info.ftLastWriteTime;
            SetFileTime(dst_handle.get(), &creation_time, &access_time, &write_time);
        }
    }

    static void move_file(const fs::path& src, const fs::path& dst, std::unordered_set<fs::path>& created_dirs) {
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
            copy_file(src, dst, created_dirs);
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
        : pool(num_threads, &processed_files, &copied_bytes, conflict_resolution, verbose, &created_dirs), 
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

                FileTask task{entry_path, dst_path, move_mode, conflict_resolution};
                
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

        FileTask task{src, dst, move_mode, conflict_resolution};
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
        std::cout << "Total time: " << std::setprecision(2) << seconds << "s" << std::endl;
    }
};

void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [OPTIONS] <source>... <destination>" << std::endl;
    std::cout << "\nOptions:" << std::endl;
    std::cout << "  -t, --threads <num>   Number of threads (default: " << DEFAULT_THREADS << ")" << std::endl;
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
    ConflictResolution conflict_resolution = ConflictResolution::FAIL;
    std::vector<std::string> src_paths;
    std::string dst_path;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-t" || arg == "--threads") {
            if (i + 1 < argc) {
                num_threads = std::stoul(argv[++i]);
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
