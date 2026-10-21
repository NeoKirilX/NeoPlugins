#include "mandagram_plugin.h"
#include "monet_wallpaper_engine_bytes.h"

#include <string>
#include <vector>
#include <mutex>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <sys/stat.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

#include <jni.h>

static const char* TAG = "WallpaperTheme";

static const PluginMetadata s_metadata = {
    "WallpaperTheme",
    "1.0.0",
    "@NeoKirilX & @NeoPlugins",
    "динамическая тема material 3 monet из обоев рабочего стола"
};

extern "C" PLUGIN_EXPORT const PluginMetadata* mandagram_plugin_metadata(void) {
    return &s_metadata;
}

static const PluginApi* g_api = nullptr;
static jobject g_class_loader = nullptr;
static std::mutex g_loader_mutex;
static std::atomic<bool> g_running{false};
static std::thread g_watcher_thread;
static std::condition_variable g_cv;
static std::mutex g_cv_mutex;

#if defined(_WIN32)
typedef jint (JNICALL *GetCreatedJavaVMs_t)(JavaVM**, jsize, jsize*);
static JavaVM* get_jvm() {
    HMODULE hJvm = GetModuleHandleA("jvm.dll");
    if (!hJvm) {
        hJvm = LoadLibraryA("jvm.dll");
    }
    if (!hJvm) return nullptr;
    GetCreatedJavaVMs_t fn = (GetCreatedJavaVMs_t)GetProcAddress(hJvm, "JNI_GetCreatedJavaVMs");
    if (!fn) return nullptr;
    JavaVM* vms[2] = {nullptr};
    jsize count = 0;
    if (fn(vms, 2, &count) == 0 && count > 0) {
        return vms[0];
    }
    return nullptr;
}
#else
typedef jint (*GetCreatedJavaVMs_t)(JavaVM**, jsize, jsize*);
static JavaVM* get_jvm() {
    void* handle = dlopen("libjvm.so", RTLD_NOLOAD);
    if (!handle) {
        handle = dlopen("libjvm.so", RTLD_LAZY);
    }
    if (!handle) return nullptr;
    GetCreatedJavaVMs_t fn = (GetCreatedJavaVMs_t)dlsym(handle, "JNI_GetCreatedJavaVMs");
    if (!fn) return nullptr;
    JavaVM* vms[2] = {nullptr};
    jsize count = 0;
    if (fn(vms, 2, &count) == 0 && count > 0) {
        return vms[0];
    }
    return nullptr;
}
#endif

static JNIEnv* get_jni_env(bool* attached_out) {
    if (attached_out) *attached_out = false;
    JavaVM* jvm = get_jvm();
    if (!jvm) return nullptr;
    JNIEnv* env = nullptr;
    jint res = jvm->GetEnv((void**)&env, JNI_VERSION_1_8);
    if (res == JNI_EDETACHED) {
        if (jvm->AttachCurrentThread((void**)&env, nullptr) == 0) {
            if (attached_out) *attached_out = true;
            return env;
        }
        return nullptr;
    }
    if (res == JNI_OK) {
        return env;
    }
    return nullptr;
}

static void detach_jni_env() {
    JavaVM* jvm = get_jvm();
    if (jvm) {
        jvm->DetachCurrentThread();
    }
}

static jclass find_app_class(JNIEnv* env, const char* class_name) {
    if (!env) return nullptr;
    std::string dotted(class_name);
    for (char& c : dotted) {
        if (c == '/') c = '.';
    }
    std::lock_guard<std::mutex> lock(g_loader_mutex);
    if (g_class_loader) {
        jclass clsLoader = env->GetObjectClass(g_class_loader);
        jmethodID midLoadClass = env->GetMethodID(clsLoader, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
        if (midLoadClass) {
            jstring jName = env->NewStringUTF(dotted.c_str());
            jclass cls = (jclass)env->CallObjectMethod(g_class_loader, midLoadClass, jName);
            env->DeleteLocalRef(jName);
            if (!env->ExceptionCheck() && cls) {
                return cls;
            }
            env->ExceptionClear();
        }
    }
    std::string slashed(class_name);
    for (char& c : slashed) {
        if (c == '.') c = '/';
    }
    jclass cls = env->FindClass(slashed.c_str());
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return nullptr;
    }
    return cls;
}

static bool file_exists(const std::string& path) {
    if (path.empty()) return false;
    struct stat st;
    return (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode));
}

static std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n'\"");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n'\"");
    return str.substr(first, (last - first + 1));
}

#if !defined(_WIN32)
static std::string expand_home(const std::string& path) {
    if (path.empty()) return path;
    if (path[0] == '~') {
        const char* home = getenv("HOME");
        if (home) return std::string(home) + path.substr(1);
    }
    return path;
}

static std::string clean_uri(const std::string& uri) {
    std::string s = trim(uri);
    if (s.rfind("file://", 0) == 0) {
        s = s.substr(7);
    }
    return expand_home(s);
}

static std::string run_cmd(const char* cmd) {
    FILE* fp = popen(cmd, "r");
    if (!fp) return "";
    char buf[1024];
    std::string out;
    while (fgets(buf, sizeof(buf), fp)) {
        out += buf;
    }
    pclose(fp);
    return trim(out);
}
#endif

static std::string detect_wallpaper() {
#if defined(_WIN32)
    wchar_t wbuf[MAX_PATH];
    if (SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, wbuf, 0)) {
        char cbuf[MAX_PATH * 4];
        int len = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, cbuf, sizeof(cbuf), nullptr, nullptr);
        if (len > 0) {
            std::string res(cbuf);
            if (file_exists(res)) return res;
        }
    }
    return "";
#else
    const char* home_env = getenv("HOME");
    std::string home = home_env ? home_env : "";

    std::string custom_conf = home + "/.config/mandagram/wallpaper_theme.conf";
    if (file_exists(custom_conf)) {
        std::ifstream ifs(custom_conf);
        std::string line;
        if (std::getline(ifs, line)) {
            std::string p = clean_uri(line);
            if (file_exists(p)) return p;
        }
    }

    std::string qs_path = home + "/.local/state/quickshell/user/generated/wallpaper/path.txt";
    if (file_exists(qs_path)) {
        std::ifstream ifs(qs_path);
        std::string line;
        if (std::getline(ifs, line)) {
            std::string p = clean_uri(line);
            if (file_exists(p)) return p;
        }
    }

    std::string ii_json = home + "/.config/illogical-impulse/config.json";
    if (file_exists(ii_json)) {
        std::ifstream ifs(ii_json);
        std::string line;
        while (std::getline(ifs, line)) {
            size_t idx = line.find("\"wallpaperPath\":");
            if (idx != std::string::npos) {
                size_t q1 = line.find('"', idx + 16);
                size_t q2 = (q1 != std::string::npos) ? line.find('"', q1 + 1) : std::string::npos;
                if (q1 != std::string::npos && q2 != std::string::npos) {
                    std::string p = clean_uri(line.substr(q1 + 1, q2 - q1 - 1));
                    if (file_exists(p)) return p;
                }
            }
        }
    }

    std::string wal_path = home + "/.cache/wal/wal";
    if (file_exists(wal_path)) {
        std::ifstream ifs(wal_path);
        std::string line;
        if (std::getline(ifs, line)) {
            std::string p = clean_uri(line);
            if (file_exists(p)) return p;
        }
    }

    std::string waypaper_ini = home + "/.config/waypaper/config.ini";
    if (file_exists(waypaper_ini)) {
        std::ifstream ifs(waypaper_ini);
        std::string line;
        while (std::getline(ifs, line)) {
            std::string t = trim(line);
            if (t.rfind("wallpaper", 0) == 0) {
                size_t eq = t.find('=');
                if (eq != std::string::npos) {
                    std::string p = clean_uri(t.substr(eq + 1));
                    if (file_exists(p)) return p;
                }
            }
        }
    }

    std::string swww_out = run_cmd("swww query 2>/dev/null");
    if (!swww_out.empty()) {
        size_t idx = swww_out.find("image: ");
        if (idx != std::string::npos) {
            std::string p = clean_uri(swww_out.substr(idx + 7));
            size_t nl = p.find_first_of("\r\n");
            if (nl != std::string::npos) p = p.substr(0, nl);
            if (file_exists(p)) return p;
        }
    }

    std::string swaybg_out = run_cmd("pgrep -a swaybg 2>/dev/null");
    if (!swaybg_out.empty()) {
        size_t idx = swaybg_out.find("-i ");
        if (idx != std::string::npos) {
            std::string p = clean_uri(swaybg_out.substr(idx + 3));
            size_t sp = p.find(' ');
            if (sp != std::string::npos) p = p.substr(0, sp);
            if (file_exists(p)) return p;
        }
    }

    std::string wbg_out = run_cmd("pgrep -a wbg 2>/dev/null");
    if (!wbg_out.empty()) {
        size_t sp = wbg_out.find(' ');
        if (sp != std::string::npos) {
            std::string p = clean_uri(wbg_out.substr(sp + 1));
            size_t nl = p.find_first_of("\r\n");
            if (nl != std::string::npos) p = p.substr(0, nl);
            if (file_exists(p)) return p;
        }
    }

    std::string hyprpaper_active = run_cmd("hyprctl hyprpaper listactive 2>/dev/null");
    if (!hyprpaper_active.empty()) {
        size_t eq = hyprpaper_active.find('=');
        if (eq != std::string::npos) {
            std::string p = clean_uri(hyprpaper_active.substr(eq + 1));
            size_t nl = p.find_first_of("\r\n");
            if (nl != std::string::npos) p = p.substr(0, nl);
            if (file_exists(p)) return p;
        }
    }

    std::string hyprpaper_conf = home + "/.config/hypr/hyprpaper.conf";
    if (file_exists(hyprpaper_conf)) {
        std::ifstream ifs(hyprpaper_conf);
        std::string line;
        while (std::getline(ifs, line)) {
            std::string t = trim(line);
            if (t.empty() || t[0] == '#') continue;
            size_t eq = t.find('=');
            if (eq != std::string::npos) {
                std::string key = trim(t.substr(0, eq));
                std::string val = trim(t.substr(eq + 1));
                if (key == "path" || key == "preload") {
                    std::string p = clean_uri(val);
                    if (file_exists(p)) return p;
                } else if (key == "wallpaper") {
                    size_t comma = val.find(',');
                    std::string p = (comma != std::string::npos) ? val.substr(comma + 1) : val;
                    p = clean_uri(p);
                    if (file_exists(p)) return p;
                }
            }
        }
    }

    std::string hypr_lock = home + "/.config/hypr/hyprlock.conf";
    if (file_exists(hypr_lock)) {
        std::ifstream ifs(hypr_lock);
        std::string line;
        while (std::getline(ifs, line)) {
            std::string t = trim(line);
            if (t.rfind("path", 0) == 0) {
                size_t eq = t.find('=');
                if (eq != std::string::npos) {
                    std::string p = clean_uri(t.substr(eq + 1));
                    if (file_exists(p)) return p;
                }
            }
        }
    }

    std::string kde_conf = home + "/.config/plasma-org.kde.plasma.desktop-appletsrc";
    if (file_exists(kde_conf)) {
        std::ifstream ifs(kde_conf);
        std::string line;
        while (std::getline(ifs, line)) {
            std::string t = trim(line);
            if (t.rfind("Image=", 0) == 0) {
                std::string p = clean_uri(t.substr(6));
                if (file_exists(p)) return p;
            } else if (t.rfind("usersWallpapers=", 0) == 0) {
                std::string p = clean_uri(t.substr(16));
                if (file_exists(p)) return p;
            }
        }
    }

    std::string cosmic_path = home + "/.config/cosmic/com.system76.CosmicBackground/v1/all";
    if (file_exists(cosmic_path)) {
        std::ifstream ifs(cosmic_path);
        std::string line;
        while (std::getline(ifs, line)) {
            size_t idx = line.find("(source: Path(\"");
            if (idx != std::string::npos) {
                size_t q2 = line.find('"', idx + 15);
                if (q2 != std::string::npos) {
                    std::string p = clean_uri(line.substr(idx + 15, q2 - idx - 15));
                    if (file_exists(p)) return p;
                }
            }
        }
    }

    std::string gnome_dark = run_cmd("gsettings get org.gnome.desktop.background picture-uri-dark 2>/dev/null");
    if (!gnome_dark.empty()) {
        std::string p = clean_uri(gnome_dark);
        if (file_exists(p)) return p;
    }
    std::string gnome_light = run_cmd("gsettings get org.gnome.desktop.background picture-uri 2>/dev/null");
    if (!gnome_light.empty()) {
        std::string p = clean_uri(gnome_light);
        if (file_exists(p)) return p;
    }

    std::string xfce_out = run_cmd("xfconf-query -c xfce4-desktop -p /backdrop/screen0/monitor0/workspace0/last-image 2>/dev/null");
    if (!xfce_out.empty()) {
        std::string p = clean_uri(xfce_out);
        if (file_exists(p)) return p;
    }

    std::string lxqt_conf = home + "/.config/pcmanfm-qt/default/settings.conf";
    if (file_exists(lxqt_conf)) {
        std::ifstream ifs(lxqt_conf);
        std::string line;
        while (std::getline(ifs, line)) {
            std::string t = trim(line);
            if (t.rfind("wallpaper=", 0) == 0) {
                std::string p = clean_uri(t.substr(10));
                if (file_exists(p)) return p;
            }
        }
    }

    std::string fehbg = home + "/.fehbg";
    if (file_exists(fehbg)) {
        std::ifstream ifs(fehbg);
        std::string line;
        while (std::getline(ifs, line)) {
            size_t q1 = line.find('\'');
            size_t q2 = (q1 != std::string::npos) ? line.rfind('\'') : std::string::npos;
            if (q1 != std::string::npos && q2 != std::string::npos && q2 > q1) {
                std::string p = clean_uri(line.substr(q1 + 1, q2 - q1 - 1));
                if (file_exists(p)) return p;
            }
        }
    }

    std::string nitrogen_cfg = home + "/.config/nitrogen/bg-saved.cfg";
    if (file_exists(nitrogen_cfg)) {
        std::ifstream ifs(nitrogen_cfg);
        std::string line;
        while (std::getline(ifs, line)) {
            std::string t = trim(line);
            if (t.rfind("file=", 0) == 0) {
                std::string p = clean_uri(t.substr(5));
                if (file_exists(p)) return p;
            }
        }
    }

    std::string hypr_default_jpg = home + "/.config/hypr/wallpaper.jpg";
    if (file_exists(hypr_default_jpg)) return hypr_default_jpg;

    std::string hypr_default_png = home + "/.config/hypr/wallpaper.png";
    if (file_exists(hypr_default_png)) return hypr_default_png;

    std::string default_bg = home + "/.wallpaper";
    if (file_exists(default_bg)) return default_bg;

    return "";
#endif
}

static std::string g_current_style = "tonal_spot";
static bool g_amoled = true;
static bool g_chroma_boost = true;

static void load_theme_config() {
    const char* home_env = getenv("HOME");
    std::string home = home_env ? home_env : "";
    std::string conf_path = home + "/.config/mandagram/monet_theme.conf";
    if (!file_exists(conf_path)) return;
    std::ifstream ifs(conf_path);
    std::string line;
    while (std::getline(ifs, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(t.substr(0, eq));
        std::string v = trim(t.substr(eq + 1));
        if (k == "style") {
            g_current_style = v;
        } else if (k == "amoled") {
            g_amoled = (v == "true" || v == "1");
        } else if (k == "chroma_boost") {
            g_chroma_boost = (v == "true" || v == "1");
        }
    }
}

static void save_theme_config() {
    const char* home_env = getenv("HOME");
    std::string home = home_env ? home_env : "";
    std::string dir = home + "/.config/mandagram";
#if defined(_WIN32)
    mkdir(dir.c_str());
#else
    mkdir(dir.c_str(), 0755);
#endif
    std::string conf_path = dir + "/monet_theme.conf";
    std::ofstream ofs(conf_path);
    if (ofs.is_open()) {
        ofs << "style=" << g_current_style << "\n";
        ofs << "amoled=" << (g_amoled ? "true" : "false") << "\n";
        ofs << "chroma_boost=" << (g_chroma_boost ? "true" : "false") << "\n";
    }
}

static int sync_wallpaper_theme() {
    load_theme_config();
    bool attached = false;
    JNIEnv* env = get_jni_env(&attached);
    if (!env) return 0;
    std::string wp = detect_wallpaper();
    if (wp.empty()) {
        if (attached) detach_jni_env();
        return 0;
    }
    jclass clsHelper = find_app_class(env, "com.mandagram.plugin.monet.MonetWallpaperEngine");
    if (!clsHelper) {
        if (attached) detach_jni_env();
        return 0;
    }
    jmethodID midUpdate = env->GetStaticMethodID(
        clsHelper,
        "updateTheme",
        "(Ljava/lang/String;Ljava/lang/String;ZZ)I"
    );
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
    if (!midUpdate) {
        if (attached) detach_jni_env();
        return 0;
    }
    jstring jPath = env->NewStringUTF(wp.c_str());
    jstring jStyle = env->NewStringUTF(g_current_style.c_str());
    jint color = env->CallStaticIntMethod(
        clsHelper,
        midUpdate,
        jPath,
        jStyle,
        (jboolean)g_amoled,
        (jboolean)g_chroma_boost
    );
    env->DeleteLocalRef(jPath);
    env->DeleteLocalRef(jStyle);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
    if (attached) detach_jni_env();

    if (g_api && color != 0) {
        char log_buf[512];
        snprintf(
            log_buf,
            sizeof(log_buf),
            "monet wallpaper updated: style=%s amoled=%d color=0x%08X file=%s",
            g_current_style.c_str(),
            g_amoled ? 1 : 0,
            (unsigned int)color,
            wp.c_str()
        );
        g_api->log_info(TAG, log_buf);
    }
    return (int)color;
}

static void on_sync_chat_action(int64_t chat_id, void* user_data) {
    (void)chat_id;
    (void)user_data;
    sync_wallpaper_theme();
}

static void on_cycle_style_action(int64_t chat_id, void* user_data) {
    (void)chat_id;
    (void)user_data;
    if (g_current_style == "tonal_spot") {
        g_current_style = "vibrant";
    } else if (g_current_style == "vibrant") {
        g_current_style = "fidelity";
    } else if (g_current_style == "fidelity") {
        g_current_style = "neutral";
    } else {
        g_current_style = "tonal_spot";
    }
    save_theme_config();
    sync_wallpaper_theme();
}

static void on_toggle_amoled_action(int64_t chat_id, void* user_data) {
    (void)chat_id;
    (void)user_data;
    g_amoled = !g_amoled;
    save_theme_config();
    sync_wallpaper_theme();
}

static void watcher_loop() {
    std::string last_path;
    time_t last_mtime = 0;
    while (g_running.load()) {
        {
            std::unique_lock<std::mutex> lk(g_cv_mutex);
            g_cv.wait_for(lk, std::chrono::seconds(2), [] { return !g_running.load(); });
        }
        if (!g_running.load()) break;
        std::string cur_path = detect_wallpaper();
        if (cur_path.empty()) continue;
        struct stat st;
        if (stat(cur_path.c_str(), &st) != 0) continue;
        if (cur_path != last_path || st.st_mtime != last_mtime) {
            last_path = cur_path;
            last_mtime = st.st_mtime;
            sync_wallpaper_theme();
        }
    }
}

extern "C" PLUGIN_EXPORT bool mandagram_plugin_init(const PluginApi* api) {
    if (!api) return false;
    g_api = api;

    g_api->log_info(TAG, "initializing monet wallpaper plugin v1.0.0");

    bool attached = false;
    JNIEnv* env = get_jni_env(&attached);
    if (!env) {
        g_api->log_error(TAG, "failed to obtain jni environment");
        return false;
    }

    jclass clsThread = env->FindClass("java/lang/Thread");
    if (!clsThread) {
        g_api->log_error(TAG, "failed to find java/lang/Thread");
        if (attached) detach_jni_env();
        return false;
    }
    jmethodID midCurrentThread = env->GetStaticMethodID(clsThread, "currentThread", "()Ljava/lang/Thread;");
    jobject curThread = env->CallStaticObjectMethod(clsThread, midCurrentThread);
    jmethodID midGetContextClassLoader = env->GetMethodID(clsThread, "getContextClassLoader", "()Ljava/lang/ClassLoader;");
    jobject classLoader = env->CallObjectMethod(curThread, midGetContextClassLoader);
    if (!classLoader) {
        g_api->log_error(TAG, "failed to get context class loader");
        if (attached) detach_jni_env();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_loader_mutex);
        if (g_class_loader) {
            env->DeleteGlobalRef(g_class_loader);
        }
        g_class_loader = env->NewGlobalRef(classLoader);
    }

    jclass clsHelper = find_app_class(env, "com.mandagram.plugin.monet.MonetWallpaperEngine");
    if (!clsHelper) {
        clsHelper = env->DefineClass(
            "com/mandagram/plugin/monet/MonetWallpaperEngine",
            g_class_loader,
            (const jbyte*)s_monet_engine_class_bytes,
            (jsize)s_monet_engine_class_bytes_len
        );
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
    }

    if (!clsHelper) {
        g_api->log_error(TAG, "failed to define MonetWallpaperEngine class");
        if (attached) detach_jni_env();
        return false;
    }

    g_api->log_info(TAG, "MonetWallpaperEngine defined successfully");

    sync_wallpaper_theme();

    g_api->register_chat_action("monet_sync", "обновить палитру из обоев", on_sync_chat_action, nullptr);
    g_api->register_chat_action("monet_style", "сменить стиль (TonalSpot/Vibrant/Fidelity)", on_cycle_style_action, nullptr);
    g_api->register_chat_action("monet_amoled", "переключить amoled фон", on_toggle_amoled_action, nullptr);

    g_running.store(true);
    g_watcher_thread = std::thread(watcher_loop);

    if (attached) detach_jni_env();

    g_api->log_info(TAG, "WallpaperTheme plugin initialized successfully");
    return true;
}

static void stop_watcher() {
    g_running.store(false);
    g_cv.notify_all();
    if (g_watcher_thread.joinable()) {
        g_watcher_thread.join();
    }
}

#if defined(_WIN32)
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpReserved) {
    (void)hinstDLL;
    (void)lpReserved;
    if (fdwReason == DLL_PROCESS_DETACH) {
        stop_watcher();
    }
    return TRUE;
}
#else
__attribute__((destructor)) static void wallpaper_theme_on_unload() {
    stop_watcher();
}
#endif
