#pragma once

#include <string>
#include <map>

enum class Language {
    ENGLISH,
    CHINESE
};

class I18N {
private:
    Language current_language;
    std::map<std::string, std::string> messages_en;
    std::map<std::string, std::string> messages_zh;
    
    void initMessages();
    
public:
    I18N();
    
    void setLanguage(Language lang);
    Language getLanguage() const;
    std::string get(const std::string& key) const;
    std::string get(const std::string& key, const std::string& param) const;
    std::string get(const std::string& key, const std::string& param1, const std::string& param2) const;
    std::string get(const std::string& key, const std::string& param1, const std::string& param2, const std::string& param3) const;
    std::string get(const std::string& key, const std::string& param1, const std::string& param2, const std::string& param3, const std::string& param4) const;
    
    static I18N& getInstance();
};

inline std::string _(const std::string& key) {
    return I18N::getInstance().get(key);
}

inline std::string _(const std::string& key, const std::string& param) {
    return I18N::getInstance().get(key, param);
}

inline std::string _(const std::string& key, const std::string& param1, const std::string& param2) {
    return I18N::getInstance().get(key, param1, param2);
}

inline std::string _(const std::string& key, const std::string& param1, const std::string& param2, const std::string& param3) {
    return I18N::getInstance().get(key, param1, param2, param3);
}

inline std::string _(const std::string& key, const std::string& param1, const std::string& param2, const std::string& param3, const std::string& param4) {
    return I18N::getInstance().get(key, param1, param2, param3, param4);
}
