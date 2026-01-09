#include <iostream>
#include <filesystem>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <string>
#include <windows.h>
#include "file_handle.h"
#include "file_operations.h"
#include "thread_pool.h"
#include "file_copier.h"
#include "i18n.h"

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
constexpr size_t PROGRESS_UPDATE_INTERVAL_MS = 100;

enum class ConflictResolution {
    OVERWRITE,
    SKIP,
    FAIL
};

class ProgressTracker {
private:
    std::atomic<size_t> total_files{0};
    std::atomic<size_t> processed_files{0};
    std::atomic<uint64_t> total_bytes{0};
    std::atomic<uint64_t> copied_bytes{0};
    std::chrono::high_resolution_clock::time_point start_time;
    std::chrono::high_resolution_clock::time_point last_progress_update;
    std::mutex cout_mutex;
    bool verbose;

public:
    ProgressTracker(bool verbose = false) : verbose(verbose) {
        start_time = std::chrono::high_resolution_clock::now();
        last_progress_update = start_time;
    }

    void reset() {
        total_files = 0;
        processed_files = 0;
        total_bytes = 0;
        copied_bytes = 0;
        start_time = std::chrono::high_resolution_clock::now();
        last_progress_update = start_time;
    }

    void add_file(uint64_t file_size) {
        total_files++;
        total_bytes += file_size;
    }

    void file_processed(uint64_t bytes_copied) {
        processed_files++;
        copied_bytes += bytes_copied;
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

    void update_scan_progress() {
        std::lock_guard<std::mutex> lock(cout_mutex);
        std::cout << "\rScanning: [" << total_files.load() << " files, " 
                  << std::fixed << std::setprecision(2) 
                  << (total_bytes.load() / (1024.0 * 1024.0)) << " MB]... ";
        std::cout.flush();
    }

    void print_summary() {
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        double seconds = duration.count() / 1000.0;
        uint64_t copied = copied_bytes.load();
        double speed = copied / (1024.0 * 1024.0) / seconds;

        std::cout << "\r" << std::string(100, ' ') << "\r";
        std::cout << "Files processed: " << processed_files.load() << std::endl;
        std::cout << "Total copied: " << std::fixed << std::setprecision(2) 
                  << (copied / (1024.0 * 1024.0)) << " MB" << std::endl;
        std::cout << "Average speed: " << std::setprecision(1) << speed << " MB/s" << std::endl;
        std::cout << "Total time: " << std::setprecision(2) << seconds << "s" << std::endl;
    }

    size_t get_processed_files() const { return processed_files.load(); }
    size_t get_total_files() const { return total_files.load(); }
    uint64_t get_copied_bytes() const { return copied_bytes.load(); }
    uint64_t get_total_bytes() const { return total_bytes.load(); }
};

void print_usage(const char* program_name) {
    std::cout << _("usage", program_name) << std::endl;
    std::cout << "\n" << _("options") << std::endl;
    std::cout << "  -t, --threads <num>   " << _("threads") << std::endl;
    std::cout << "  -m, --mode <mode>     " << _("mode") << std::endl;
    std::cout << "  -v, --verbose         " << _("verbose") << std::endl;
    std::cout << "  -o, --overwrite       " << _("overwrite") << std::endl;
    std::cout << "  -s, --skip            " << _("skip") << std::endl;
    std::cout << "  -V, --version         " << _("version") << std::endl;
    std::cout << "  -h, --help            " << _("help") << std::endl;
}

void print_version() {
    std::string version_str = VERSION;
    std::string commit_str = GIT_COMMIT;
    std::string date_str = BUILD_DATE;
    std::cout << _("version_info", "syncmt", version_str, commit_str, date_str) << std::endl;
}

Language detect_system_language() {
    LANGID lang_id = GetSystemDefaultLangID();
    WORD primary_lang = PRIMARYLANGID(lang_id);
    
    if (primary_lang == LANG_CHINESE) {
        return Language::CHINESE;
    }
    return Language::ENGLISH;
}

int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    
    I18N& i18n = I18N::getInstance();
    Language system_lang = detect_system_language();
    i18n.setLanguage(system_lang);
    
    size_t num_threads = DEFAULT_THREADS;
    CopyMode copy_mode = CopyMode::MEMORY_MAPPED;
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
        } else if (arg == "-m" || arg == "--mode") {
            if (i + 1 < argc) {
                std::string mode = argv[++i];
                if (mode == "mmap" || mode == "memory-mapped") {
                    copy_mode = CopyMode::MEMORY_MAPPED;
                } else if (mode == "async" || mode == "async-io") {
                    copy_mode = CopyMode::ASYNC_IO;
                } else if (mode == "standard" || mode == "sync") {
                    copy_mode = CopyMode::STANDARD;
                } else {
                    std::cerr << "Unknown copy mode: " << mode << std::endl;
                    print_usage(argv[0]);
                    return 1;
                }
            }
        } else if (arg == "--lang") {
            if (i + 1 < argc) {
                std::string lang = argv[++i];
                if (lang == "en" || lang == "english") {
                    i18n.setLanguage(Language::ENGLISH);
                } else if (lang == "zh" || lang == "chinese") {
                    i18n.setLanguage(Language::CHINESE);
                } else {
                    std::cerr << "Unknown language: " << lang << std::endl;
                    std::cerr << "Supported languages: en, zh" << std::endl;
                    return 1;
                }
            }
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
        if (!file_exists(src)) {
            std::cerr << _("error_source", src) << std::endl;
            return 1;
        }
    }

    try {
        switch (copy_mode) {
            case CopyMode::MEMORY_MAPPED:
                std::cout << _("mode_mmap") << std::endl;
                break;
            case CopyMode::ASYNC_IO:
                std::cout << _("mode_async") << std::endl;
                break;
            case CopyMode::STANDARD:
                std::cout << _("mode_standard") << std::endl;
                break;
        }
        std::cout << _("thread_count", std::to_string(num_threads)) << std::endl;
        std::cout << "Source(s): ";
        for (size_t i = 0; i < src_paths.size(); ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << src_paths[i];
        }
        std::cout << std::endl;
        std::cout << "Destination: " << dst_path << std::endl;
        std::cout << "Threads: " << num_threads << std::endl;
        std::cout << "Copy mode: ";
        switch (copy_mode) {
            case CopyMode::MEMORY_MAPPED:
                std::cout << "memory-mapped" << std::endl;
                break;
            case CopyMode::ASYNC_IO:
                std::cout << "async I/O" << std::endl;
                break;
            case CopyMode::STANDARD:
                std::cout << "standard" << std::endl;
                break;
        }
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

        FileCopier copier(num_threads, copy_mode);

        std::vector<fs::path> sources;
        for (const auto& src : src_paths) {
            sources.push_back(src);
        }

        copier.copy(sources.size() == 1 ? sources[0] : sources.front(), dst_path);

        std::cout << "\nOperation completed successfully!" << std::endl;
        copier.print_summary();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}
