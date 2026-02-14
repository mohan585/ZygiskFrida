#include <string>
#include <thread>

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

        std::string module_dir = std::string("/data/local/tmp/re.zyg.fri");
        this->cfg = load_config(module_dir, app_name);
        
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
