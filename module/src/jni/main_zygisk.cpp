#include <string>
#include <thread>

#include <unistd.h>
#include <limits.h>

#include "inject.h"
#include "log.h"
#include "zygisk.h"

using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ServerSpecializeArgs;

class MyModule : public zygisk::ModuleBase {
 public:
    void onLoad(Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
    }



    void preAppSpecialize(AppSpecializeArgs *args) override {
        const char *raw_app_name = env->GetStringUTFChars(args->nice_name, nullptr);
        std::string app_name = std::string(raw_app_name);
        this->env->ReleaseStringUTFChars(args->nice_name, raw_app_name);

        std::string module_dir = std::string("/data/adb/modules/zygiskfrida");
        int module_fd = api->getModuleDir();

        if (module_fd >= 0) {
            char path[PATH_MAX];
            char fd_path[64];
            snprintf(fd_path, sizeof(fd_path), "/proc/self/fd/%d", module_fd);
            ssize_t len = readlink(fd_path, path, sizeof(path) - 1);
            if (len != -1) {
                path[len] = '\0';
                LOGI("Pre-app spec: module_fd %d points to %s", module_fd, path);
            }
        }

        this->cfg = load_config(module_dir, module_fd, app_name);
        
        if (!this->cfg.has_value()) {
            LOGI("Pre-app spec: FD load failed, trying path fallback: %s", module_dir.c_str());
            this->cfg = load_config(module_dir, -1, app_name);
        }

        if (this->cfg.has_value()) {
            LOGI("Pre-app spec: Loaded config for %s", app_name.c_str());
        }
    }

    void postAppSpecialize(const AppSpecializeArgs *args) override {
        if (this->cfg.has_value() && this->cfg->enabled) {
            LOGI("App spec: INJECTION STARTED");
            std::thread inject_thread(start_injection, this->cfg.value());
            inject_thread.detach();
        } else {
            this->api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
        }
    }

 private:
    Api *api;
    JNIEnv *env;
    std::optional<target_config> cfg;
};

REGISTER_ZYGISK_MODULE(MyModule)
