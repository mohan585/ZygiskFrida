#include "config.h"

#include <string>
#include <fstream>
#include <sstream>
#include <optional>
#include <fcntl.h>
#include <unistd.h>

#include "rapidjson/document.h"
#include "rapidjson/istreamwrapper.h"
#include "rapidjson/error/en.h"
#include "log.h"

// Helper to read file from FD or path
static std::optional<std::string> read_file_content(std::string const &path, int dir_fd, std::string const &filename, bool quiet = false) {
    if (dir_fd > 0) {
        int fd = openat(dir_fd, filename.c_str(), O_RDONLY);
        if (fd < 0) {
             if (!quiet) LOGD("Failed to openat config: %s (errno=%d)", filename.c_str(), errno);
             return std::nullopt;
        }
        std::string content;
        char buffer[1024];
        ssize_t bytes_read;
        while ((bytes_read = read(fd, buffer, sizeof(buffer))) > 0) {
            content.append(buffer, bytes_read);
        }
        close(fd);
        return content;
    } else {
        std::ifstream file(path);
        if (!file.is_open()) {
             if (!quiet) LOGD("Failed to open config: %s (errno=%d)", path.c_str(), errno);
             return std::nullopt;
        }
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }
}

// This must work in a non-exception environment as the libcxx we use don't have support for exception.
// Should avoid any libraries aborting on parse error as this will run on every app start and
// might otherwise cause issues for any app starting if misconfigured.

static std::optional<std::vector<std::string>> deserialize_libraries(const rapidjson::Value &doc) {
    if (!doc.IsArray()) {
        LOGE("invalid config: expected injected_libraries to be an array");
        return std::nullopt;
    }

    std::vector<std::string> result;

    for (rapidjson::SizeType i = 0; i < doc.Size(); i++) {
        auto &library = doc[i];
        if (!library.IsObject()) {
            LOGE("invalid config: expected injected_libraries members to be objects");
            return std::nullopt;
        }

        auto &path = library["path"];
        if (!path.IsString()) {
            LOGE("invalid config: expected injected_libraries.path to be a string");
            return std::nullopt;
        }

        result.emplace_back(path.GetString());
    }

    return result;
}

static std::optional<child_gating_config> deserialize_child_gating_config(const rapidjson::Value &doc) {
    if (!doc.IsObject()) {
        return std::nullopt;
    }

    child_gating_config result = {};

    auto &enabled = doc["enabled"];
    if (!enabled.IsBool()) {
        LOGE("invalid config: expected child_gating.enabled members to be a bool");
        return std::nullopt;
    }
    result.enabled = enabled.GetBool();

    auto &mode = doc["mode"];
    if (!mode.IsString()) {
        LOGE("invalid config: expected child_gating.mode members to be a string");
        return std::nullopt;
    }
    result.mode = mode.GetString();

    if (doc.HasMember("injected_libraries")) {
        auto injected_libraries = deserialize_libraries(doc["injected_libraries"]);
        if (!injected_libraries.has_value()) {
            return std::nullopt;
        }
        result.injected_libraries = injected_libraries.value();
    }

    return result;
}

static std::optional<target_config> deserialize_target_config(const rapidjson::Value &doc) {
    if (!doc.IsObject()) {
        LOGE("expected config targets array to contain objects");
        return std::nullopt;
    }

    target_config result = {};

    auto &app_name = doc["app_name"];
    if (!app_name.IsString()) {
        LOGE("expected config target to have a valid app_name");
        return std::nullopt;
    }
    result.app_name = app_name.GetString();

    auto &enabled = doc["enabled"];
    if (!enabled.IsBool()) {
        LOGE("invalid config: expected targets.enabled members to be a bool");
        return std::nullopt;
    }
    result.enabled = enabled.GetBool();

    auto &start_up_delay_ms = doc["start_up_delay_ms"];
    if (!start_up_delay_ms.IsUint64()) {
        LOGE("expected config target start_up_delay_ms to be an uint64");
        return std::nullopt;
    }
    result.start_up_delay_ms = start_up_delay_ms.GetUint64();

    auto &injected_libaries = doc["injected_libraries"];
    auto deserialized_libraries = deserialize_libraries(injected_libaries);
    if (!deserialized_libraries.has_value()) {
        return std::nullopt;
    }
    result.injected_libraries = deserialized_libraries.value();

    if (doc.HasMember("child_gating")) {
        auto child_gating = deserialize_child_gating_config(doc["child_gating"]);
        if (!child_gating.has_value()) {
            return std::nullopt;
        }
        result.child_gating = child_gating.value();
    }

    return result;
}

static std::vector<std::string> split(std::string const &str, char delimiter) {
    std::vector<std::string> result;
    std::stringstream ss(str);

    std::string tmp;
    while (getline(ss, tmp, delimiter)) {
        result.push_back(tmp);
    }

    return result;
}

static std::vector<std::string> parse_injected_libraries(std::string const &module_dir, int module_dir_fd) {
    auto content_opt = read_file_content(module_dir + "/injected_libraries", module_dir_fd, "injected_libraries", true);
    
    if (!content_opt.has_value()) {
        return {module_dir + "/libgadget.so"};
    }

    std::vector<std::string> injected_libraries;
    std::stringstream ss(content_opt.value());
    std::string libpath;
    while (getline(ss, libpath)) {
        if (!libpath.empty()) {
            injected_libraries.push_back(libpath);
        }
    }

    return injected_libraries;
}

static std::optional<target_config> load_simple_config(std::string const &module_dir, int module_dir_fd, std::string const &app_name) {
    auto content_opt = read_file_content(module_dir + "/target_packages", module_dir_fd, "target_packages", true);
    
    if (!content_opt.has_value()) {
        return std::nullopt;
    }

    std::stringstream ss(content_opt.value());
    std::string line;
    while (getline(ss, line)) {
        if (line.empty()) {
            continue;
        }

        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        auto splitted = split(line, ',');
        if (splitted[0] != app_name) {
            LOGD("Simple config mismatch: '%s' != '%s'", splitted[0].c_str(), app_name.c_str());
            continue;
        }

        target_config cfg = {};
        cfg.app_name = splitted[0];
        cfg.enabled = true;
        if (splitted.size() >= 2) {
            cfg.start_up_delay_ms = std::strtoul(splitted[1].c_str(), nullptr, 10);
        }
        cfg.injected_libraries = parse_injected_libraries(module_dir, module_dir_fd);

        return cfg;
    }

    return std::nullopt;
}

static std::optional<target_config> load_advanced_config(std::string const &module_dir, int module_dir_fd, std::string const &app_name) {
    auto content_opt = read_file_content(module_dir + "/config.json", module_dir_fd, "config.json");
    
    if (!content_opt.has_value()) {
        return std::nullopt;
    }

    rapidjson::Document doc;
    doc.Parse(content_opt.value().c_str());

    if (doc.HasParseError()) {
        LOGE("config is not a valid json file offset %u: %s",
             (unsigned) doc.GetErrorOffset(),
             GetParseError_En(doc.GetParseError()));
        return std::nullopt;
    }

    if (!doc.IsObject()) {
        LOGE("config expected a json root object");
        return std::nullopt;
    }

    auto &targets = doc["targets"];
    if (!targets.IsArray()) {
        LOGE("expected config targets to be an array");
        return std::nullopt;
    }

    for (rapidjson::SizeType i = 0; i < targets.Size(); i++) {
        auto deserialized_target = deserialize_target_config(targets[i]);
        if (!deserialized_target.has_value()) {
            return std::nullopt;
        }

        auto target = deserialized_target.value();
        if (target.app_name == app_name) {
            LOGI("Config match found for app: %s", app_name.c_str());
            return target;
        }
    }

    // LOGD("Config loaded but no match for app: %s", app_name.c_str());
    return std::nullopt;

    return std::nullopt;
}

std::optional<target_config> load_config(std::string const &module_dir, int module_dir_fd, std::string const &app_name) {
    auto cfg = load_advanced_config(module_dir, module_dir_fd, app_name);
    if (cfg.has_value()) {
        return cfg;
    }

    return load_simple_config(module_dir, module_dir_fd, app_name);
}
