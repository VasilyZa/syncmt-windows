#include "i18n.h"
#include <sstream>

I18N::I18N() : current_language(Language::ENGLISH) {
    initMessages();
}

void I18N::initMessages() {
    messages_en = {
        {"usage", "Usage: %s [OPTIONS] <source>... <destination>"},
        {"options", "Options:"},
        {"threads", "Number of threads (default: 4)"},
        {"mode", "Copy mode: mmap, async, standard (default: mmap)"},
        {"verbose", "Enable verbose output"},
        {"overwrite", "Overwrite existing files"},
        {"skip", "Skip existing files"},
        {"version", "Print version information"},
        {"help", "Show this help message"},
        {"version_info", "%s version %s\nGit commit: %s\nBuild date: %s"},
        {"copying_file", "Copying %s to %s"},
        {"copying_dir", "Copying directory %s to %s"},
        {"skipping_file", "Skipping existing file: %s"},
        {"creating_dir", "Creating directory: %s"},
        {"summary", "Copy summary:"},
        {"files_copied", "Files copied: %llu"},
        {"dirs_created", "Directories created: %llu"},
        {"bytes_copied", "Total bytes copied: %llu"},
        {"errors", "Errors: %llu"},
        {"error_details", "Error details:"},
        {"error_copying", "Error copying %s to %s: %s"},
        {"error_dir", "Error copying directory %s: %s"},
        {"error_source", "Source path does not exist: %s"},
        {"error_disk_space", "Insufficient disk space for: %s"},
        {"error_open_src", "Failed to open source file: %s"},
        {"error_create_dst", "Failed to create destination file: %s"},
        {"error_read", "Failed to read file: %s"},
        {"error_write", "Failed to write file: %s"},
        {"error_incomplete", "Write incomplete: %s"},
        {"mode_mmap", "Using memory-mapped copy mode"},
        {"mode_async", "Using async I/O copy mode"},
        {"mode_standard", "Using standard copy mode"},
        {"thread_count", "Using %d threads"},
        {"completed", "Copy completed successfully"},
        {"failed", "Copy completed with errors"}
    };
    
    messages_zh = {
        {"usage", "用法: %s [选项] <源文件>... <目标目录>"},
        {"options", "选项:"},
        {"threads", "线程数量 (默认: 4)"},
        {"mode", "复制模式: mmap, async, standard (默认: mmap)"},
        {"verbose", "启用详细输出"},
        {"overwrite", "覆盖现有文件"},
        {"skip", "跳过现有文件"},
        {"version", "打印版本信息"},
        {"help", "显示此帮助信息"},
        {"version_info", "%s 版本 %s\nGit 提交: %s\n构建日期: %s"},
        {"copying_file", "正在复制 %s 到 %s"},
        {"copying_dir", "正在复制目录 %s 到 %s"},
        {"skipping_file", "跳过现有文件: %s"},
        {"creating_dir", "创建目录: %s"},
        {"summary", "复制摘要:"},
        {"files_copied", "已复制文件数: %llu"},
        {"dirs_created", "已创建目录数: %llu"},
        {"bytes_copied", "已复制总字节数: %llu"},
        {"errors", "错误数: %llu"},
        {"error_details", "错误详情:"},
        {"error_copying", "复制 %s 到 %s 时出错: %s"},
        {"error_dir", "复制目录 %s 时出错: %s"},
        {"error_source", "源路径不存在: %s"},
        {"error_disk_space", "磁盘空间不足: %s"},
        {"error_open_src", "无法打开源文件: %s"},
        {"error_create_dst", "无法创建目标文件: %s"},
        {"error_read", "读取文件失败: %s"},
        {"error_write", "写入文件失败: %s"},
        {"error_incomplete", "写入不完整: %s"},
        {"mode_mmap", "使用内存映射复制模式"},
        {"mode_async", "使用异步I/O复制模式"},
        {"mode_standard", "使用标准复制模式"},
        {"thread_count", "使用 %d 个线程"},
        {"completed", "复制成功完成"},
        {"failed", "复制完成，但有错误"}
    };
}

void I18N::setLanguage(Language lang) {
    current_language = lang;
}

Language I18N::getLanguage() const {
    return current_language;
}

std::string I18N::get(const std::string& key) const {
    const auto& messages = (current_language == Language::CHINESE) ? messages_zh : messages_en;
    auto it = messages.find(key);
    if (it != messages.end()) {
        return it->second;
    }
    return key;
}

std::string I18N::get(const std::string& key, const std::string& param) const {
    std::string format = get(key);
    size_t pos = format.find("%s");
    if (pos != std::string::npos) {
        return format.substr(0, pos) + param + format.substr(pos + 2);
    }
    pos = format.find("%llu");
    if (pos != std::string::npos) {
        return format.substr(0, pos) + param + format.substr(pos + 4);
    }
    pos = format.find("%d");
    if (pos != std::string::npos) {
        return format.substr(0, pos) + param + format.substr(pos + 2);
    }
    return format;
}

std::string I18N::get(const std::string& key, const std::string& param1, const std::string& param2) const {
    std::string format = get(key);
    
    size_t pos1 = format.find("%s");
    if (pos1 == std::string::npos) {
        pos1 = format.find("%llu");
    }
    if (pos1 == std::string::npos) {
        pos1 = format.find("%d");
    }
    
    if (pos1 != std::string::npos) {
        size_t pos2 = format.find("%s", pos1 + 2);
        if (pos2 == std::string::npos) {
            pos2 = format.find("%llu", pos1 + 2);
        }
        if (pos2 == std::string::npos) {
            pos2 = format.find("%d", pos1 + 2);
        }
        
        if (pos2 != std::string::npos) {
            int skip_length = (format[pos1 + 1] == 's') ? 2 : ((format[pos1 + 1] == 'd') ? 2 : 4);
            int skip_length2 = (format[pos2 + 1] == 's') ? 2 : ((format[pos2 + 1] == 'd') ? 2 : 4);
            return format.substr(0, pos1) + param1 + format.substr(pos1 + skip_length, pos2 - pos1 - skip_length) + param2 + format.substr(pos2 + skip_length2);
        }
        return format.substr(0, pos1) + param1 + format.substr(pos1 + 2);
    }
    return format;
}

std::string I18N::get(const std::string& key, const std::string& param1, const std::string& param2, const std::string& param3) const {
    std::string format = get(key);
    
    size_t pos1 = format.find("%s");
    if (pos1 == std::string::npos) {
        pos1 = format.find("%llu");
    }
    if (pos1 == std::string::npos) {
        pos1 = format.find("%d");
    }
    
    if (pos1 != std::string::npos) {
        size_t pos2 = format.find("%s", pos1 + 2);
        if (pos2 == std::string::npos) {
            pos2 = format.find("%llu", pos1 + 2);
        }
        if (pos2 == std::string::npos) {
            pos2 = format.find("%d", pos1 + 2);
        }
        
        if (pos2 != std::string::npos) {
            size_t pos3 = format.find("%s", pos2 + 2);
            if (pos3 == std::string::npos) {
                pos3 = format.find("%llu", pos2 + 2);
            }
            if (pos3 == std::string::npos) {
                pos3 = format.find("%d", pos2 + 2);
            }
            
            if (pos3 != std::string::npos) {
                int skip_length1 = (format[pos1 + 1] == 's') ? 2 : ((format[pos1 + 1] == 'd') ? 2 : 4);
                int skip_length2 = (format[pos2 + 1] == 's') ? 2 : ((format[pos2 + 1] == 'd') ? 2 : 4);
                int skip_length3 = (format[pos3 + 1] == 's') ? 2 : ((format[pos3 + 1] == 'd') ? 2 : 4);
                return format.substr(0, pos1) + param1 + format.substr(pos1 + skip_length1, pos2 - pos1 - skip_length1) + param2 + format.substr(pos2 + skip_length2, pos3 - pos2 - skip_length2) + param3 + format.substr(pos3 + skip_length3);
            }
            int skip_length1 = (format[pos1 + 1] == 's') ? 2 : ((format[pos1 + 1] == 'd') ? 2 : 4);
            int skip_length2 = (format[pos2 + 1] == 's') ? 2 : ((format[pos2 + 1] == 'd') ? 2 : 4);
            return format.substr(0, pos1) + param1 + format.substr(pos1 + skip_length1, pos2 - pos1 - skip_length1) + param2 + format.substr(pos2 + skip_length2);
        }
        int skip_length1 = (format[pos1 + 1] == 's') ? 2 : ((format[pos1 + 1] == 'd') ? 2 : 4);
        return format.substr(0, pos1) + param1 + format.substr(pos1 + skip_length1);
    }
    return format;
}

std::string I18N::get(const std::string& key, const std::string& param1, const std::string& param2, const std::string& param3, const std::string& param4) const {
    std::string format = get(key);
    
    size_t pos1 = format.find("%s");
    if (pos1 == std::string::npos) {
        pos1 = format.find("%llu");
    }
    if (pos1 == std::string::npos) {
        pos1 = format.find("%d");
    }
    
    if (pos1 != std::string::npos) {
        size_t pos2 = format.find("%s", pos1 + 2);
        if (pos2 == std::string::npos) {
            pos2 = format.find("%llu", pos1 + 2);
        }
        if (pos2 == std::string::npos) {
            pos2 = format.find("%d", pos1 + 2);
        }
        
        if (pos2 != std::string::npos) {
            size_t pos3 = format.find("%s", pos2 + 2);
            if (pos3 == std::string::npos) {
                pos3 = format.find("%llu", pos2 + 2);
            }
            if (pos3 == std::string::npos) {
                pos3 = format.find("%d", pos2 + 2);
            }
            
            if (pos3 != std::string::npos) {
                size_t pos4 = format.find("%s", pos3 + 2);
                if (pos4 == std::string::npos) {
                    pos4 = format.find("%llu", pos3 + 2);
                }
                if (pos4 == std::string::npos) {
                    pos4 = format.find("%d", pos3 + 2);
                }
                
                if (pos4 != std::string::npos) {
                    int skip_length1 = (format[pos1 + 1] == 's') ? 2 : ((format[pos1 + 1] == 'd') ? 2 : 4);
                    int skip_length2 = (format[pos2 + 1] == 's') ? 2 : ((format[pos2 + 1] == 'd') ? 2 : 4);
                    int skip_length3 = (format[pos3 + 1] == 's') ? 2 : ((format[pos3 + 1] == 'd') ? 2 : 4);
                    int skip_length4 = (format[pos4 + 1] == 's') ? 2 : ((format[pos4 + 1] == 'd') ? 2 : 4);
                    return format.substr(0, pos1) + param1 + format.substr(pos1 + skip_length1, pos2 - pos1 - skip_length1) + param2 + format.substr(pos2 + skip_length2, pos3 - pos2 - skip_length2) + param3 + format.substr(pos3 + skip_length3, pos4 - pos3 - skip_length3) + param4 + format.substr(pos4 + skip_length4);
                }
                int skip_length1 = (format[pos1 + 1] == 's') ? 2 : ((format[pos1 + 1] == 'd') ? 2 : 4);
                int skip_length2 = (format[pos2 + 1] == 's') ? 2 : ((format[pos2 + 1] == 'd') ? 2 : 4);
                int skip_length3 = (format[pos3 + 1] == 's') ? 2 : ((format[pos3 + 1] == 'd') ? 2 : 4);
                return format.substr(0, pos1) + param1 + format.substr(pos1 + skip_length1, pos2 - pos1 - skip_length1) + param2 + format.substr(pos2 + skip_length2, pos3 - pos2 - skip_length2) + param3 + format.substr(pos3 + skip_length3);
            }
            int skip_length1 = (format[pos1 + 1] == 's') ? 2 : ((format[pos1 + 1] == 'd') ? 2 : 4);
            int skip_length2 = (format[pos2 + 1] == 's') ? 2 : ((format[pos2 + 1] == 'd') ? 2 : 4);
            return format.substr(0, pos1) + param1 + format.substr(pos1 + skip_length1, pos2 - pos1 - skip_length1) + param2 + format.substr(pos2 + skip_length2);
        }
        int skip_length1 = (format[pos1 + 1] == 's') ? 2 : ((format[pos1 + 1] == 'd') ? 2 : 4);
        return format.substr(0, pos1) + param1 + format.substr(pos1 + skip_length1);
    }
    return format;
}

I18N& I18N::getInstance() {
    static I18N instance;
    return instance;
}
