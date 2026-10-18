#include "mandagram_plugin.h"

#include <string>
#include <vector>
#include <unordered_set>
#include <mutex>
#include <thread>
#include <cstring>
#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <jni.h>

static const char* TAG = "exmoji";

static const PluginMetadata s_metadata = {
    "exmoji",
    "1.0.0",
    "@NeoKirilX & @NeoPlugins",
    "НЕ ЭКСТЕРА ЭМОДЗИ А ЭМОДЗИ БЫВШЕГО"
};

extern "C" PLUGIN_EXPORT const PluginMetadata* mandagram_plugin_metadata(void) {
    return &s_metadata;
}

static const PluginApi* g_api = nullptr;
static jobject g_class_loader = nullptr;
static std::mutex g_loader_mutex;

static std::unordered_set<uint64_t> s_requested_emojis;
static std::mutex s_requested_mutex;

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
    if (!handle) handle = dlopen("libjvm.so", RTLD_LAZY);
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

static JNIEnv* get_jni_env() {
    JavaVM* vm = get_jvm();
    if (!vm) return nullptr;
    JNIEnv* env = nullptr;
    jint res = vm->GetEnv((void**)&env, JNI_VERSION_1_8);
    if (res == JNI_EDETACHED) {
        res = vm->AttachCurrentThread((void**)&env, nullptr);
    }
    if (res == JNI_OK) return env;
    return nullptr;
}

static jclass find_app_class(JNIEnv* env, const char* name_slashes) {
    if (!env) return nullptr;
    
    std::string dotted = name_slashes;
    for (char& c : dotted) {
        if (c == '/') c = '.';
    }
    
    if (g_class_loader) {
        jclass clsClassLoader = env->FindClass("java/lang/ClassLoader");
        if (clsClassLoader) {
            jmethodID midLoadClass = env->GetMethodID(clsClassLoader, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
            if (midLoadClass) {
                jstring jname = env->NewStringUTF(dotted.c_str());
                jclass cls = (jclass)env->CallObjectMethod(g_class_loader, midLoadClass, jname);
                env->DeleteLocalRef(jname);
                if (!env->ExceptionCheck() && cls) {
                    return cls;
                }
                env->ExceptionClear();
            }
        }
    }
    
    jclass sysCls = env->FindClass(name_slashes);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return nullptr;
    }
    return sysCls;
}

static bool parse_emoji_id_from_url(const char* url, uint64_t& out_id) {
    if (!url) return false;
    const char* p = strstr(url, "emoji?id=");
    if (!p) p = strstr(url, "id=");
    if (!p) return false;
    p += (strstr(url, "emoji?id=") ? 9 : 3);
    char* endptr = nullptr;
    out_id = strtoull(p, &endptr, 10);
    return (endptr != p && out_id != 0);
}

struct EmojiMatch {
    std::string emoji;
    uint64_t emoji_id;
    size_t utf16_offset;
    size_t utf16_length;
};

static size_t utf8_to_utf16_len(const char* s, size_t byte_count) {
    size_t utf16_len = 0;
    size_t i = 0;
    while (i < byte_count && s[i] != '\0') {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            utf16_len += 1;
            i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            utf16_len += 1;
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            utf16_len += 1;
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            utf16_len += 2;
            i += 4;
        } else {
            i += 1;
        }
    }
    return utf16_len;
}

static size_t prev_utf8_char(const std::string& str, size_t pos) {
    if (pos == 0) return 0;
    size_t i = pos - 1;
    while (i > 0 && ((unsigned char)str[i] & 0xC0) == 0x80) {
        --i;
    }
    return i;
}

static size_t find_emoji_cluster_start(const std::string& str, size_t end_pos) {
    if (end_pos == 0) return 0;
    size_t cur = prev_utf8_char(str, end_pos);
    while (cur > 0) {
        size_t prev = prev_utf8_char(str, cur);
        std::string codepoint = str.substr(prev, cur - prev);
        if (codepoint == "\xEF\xB8\x8F" || codepoint == "\xE2\x80\x8D" ||
            (codepoint.size() == 4 && (unsigned char)codepoint[0] == 0xF0 && (unsigned char)codepoint[1] == 0x9F)) {
            cur = prev;
        } else {
            break;
        }
    }
    return cur;
}

static bool transform_custom_emojis(const std::string& input, std::string& out_text, std::vector<EmojiMatch>& out_matches) {
    out_text.clear();
    out_matches.clear();
    
    size_t last_pos = 0;
    size_t current_utf16_len = 0;
    size_t pos = 0;
    
    while ((pos = input.find("tg://emoji?id=", pos)) != std::string::npos) {
        size_t id_start = pos + 14;
        char* endptr = nullptr;
        uint64_t emoji_id = strtoull(&input[id_start], &endptr, 10);
        if (endptr == &input[id_start]) {
            pos = id_start;
            continue;
        }
        
        size_t url_end = (endptr - input.data());
        bool has_opening_paren = (pos > 0 && input[pos - 1] == '(');
        if (has_opening_paren && url_end < input.size() && input[url_end] == ')') {
            url_end += 1;
        }
        
        size_t pattern_start = has_opening_paren ? (pos - 1) : pos;
        bool is_markdown = false;
        size_t chunk_end_before_emoji = pattern_start;
        std::string emoji_str;
        
        if (pattern_start > 0 && input[pattern_start - 1] == ']') {
            size_t bracket_start = input.rfind('[', pattern_start - 2);
            if (bracket_start != std::string::npos && bracket_start >= last_pos) {
                is_markdown = true;
                chunk_end_before_emoji = bracket_start;
                emoji_str = input.substr(bracket_start + 1, (pattern_start - 1) - (bracket_start + 1));
            }
        }
        
        if (!is_markdown) {
            size_t emoji_start = find_emoji_cluster_start(input, pattern_start);
            if (emoji_start >= last_pos && emoji_start < pattern_start) {
                chunk_end_before_emoji = emoji_start;
                emoji_str = input.substr(emoji_start, pattern_start - emoji_start);
            } else {
                emoji_str = "🌟";
                chunk_end_before_emoji = pattern_start;
            }
        }
        
        if (emoji_str.empty()) {
            emoji_str = "🌟";
        }
        
        if (chunk_end_before_emoji > last_pos) {
            std::string before_chunk = input.substr(last_pos, chunk_end_before_emoji - last_pos);
            current_utf16_len += utf8_to_utf16_len(before_chunk.data(), before_chunk.size());
            out_text += before_chunk;
        }
        
        size_t emoji_utf16_offset = current_utf16_len;
        size_t emoji_utf16_len = utf8_to_utf16_len(emoji_str.data(), emoji_str.size());
        
        out_matches.push_back({emoji_str, emoji_id, emoji_utf16_offset, emoji_utf16_len});
        
        out_text += emoji_str;
        current_utf16_len += emoji_utf16_len;
        
        last_pos = url_end;
        pos = url_end;
    }
    
    if (last_pos < input.length()) {
        std::string remainder = input.substr(last_pos);
        out_text += remainder;
    }
    
    return !out_matches.empty();
}

static void async_request_custom_emoji_stickers(const std::vector<uint64_t>& ids_to_request) {
    if (!g_api || !g_api->execute_tdlib_query) return;
    
    std::thread([ids = ids_to_request]() {
        for (uint64_t id : ids) {
            char query[256];
            snprintf(query, sizeof(query),
                     "{\"@type\":\"getCustomEmojiStickers\",\"custom_emoji_ids\":[%llu]}",
                     (unsigned long long)id);
            char resp_buf[2048];
            g_api->execute_tdlib_query(query, resp_buf, sizeof(resp_buf));
        }
    }).detach();
}

static void process_formatted_text(JNIEnv* env, jobject fmtText) {
    if (!env || !fmtText) return;
    
    jclass clsFmtText = env->GetObjectClass(fmtText);
    jfieldID fidText = env->GetFieldID(clsFmtText, "text", "Ljava/lang/String;");
    jfieldID fidEntities = env->GetFieldID(clsFmtText, "entities", "[Lit/tdlight/jni/TdApi$TextEntity;");
    if (!fidText || !fidEntities) {
        env->ExceptionClear();
        return;
    }
    
    jstring jstrText = (jstring)env->GetObjectField(fmtText, fidText);
    jobjectArray jarrEntities = (jobjectArray)env->GetObjectField(fmtText, fidEntities);
    
    jclass clsTextEntity = find_app_class(env, "it/tdlight/jni/TdApi$TextEntity");
    jclass clsTextUrl = find_app_class(env, "it/tdlight/jni/TdApi$TextEntityTypeTextUrl");
    jclass clsCustomEmoji = find_app_class(env, "it/tdlight/jni/TdApi$TextEntityTypeCustomEmoji");
    
    if (!clsTextEntity || !clsTextUrl || !clsCustomEmoji) {
        if (g_api) g_api->log_error(TAG, "Failed to load TDLib entity classes via app ClassLoader");
        return;
    }
    
    jmethodID ctorCustomEmoji = env->GetMethodID(clsCustomEmoji, "<init>", "(J)V");
    jmethodID ctorTextEntity = env->GetMethodID(clsTextEntity, "<init>", "(IILit/tdlight/jni/TdApi$TextEntityType;)V");
    jfieldID fidEntityType = env->GetFieldID(clsTextEntity, "type", "Lit/tdlight/jni/TdApi$TextEntityType;");
    jfieldID fidEntityLength = env->GetFieldID(clsTextEntity, "length", "I");
    jfieldID fidUrl = env->GetFieldID(clsTextUrl, "url", "Ljava/lang/String;");
    
    if (!ctorCustomEmoji || !ctorTextEntity || !fidEntityType || !fidUrl) {
        env->ExceptionClear();
        return;
    }
    
    bool modified = false;
    std::vector<uint64_t> ids_to_request;
    
    if (jarrEntities) {
        jsize count = env->GetArrayLength(jarrEntities);
        for (jsize i = 0; i < count; ++i) {
            jobject entity = env->GetObjectArrayElement(jarrEntities, i);
            if (!entity) continue;
            jobject typeObj = env->GetObjectField(entity, fidEntityType);
            if (!typeObj) continue;
            
            if (env->IsInstanceOf(typeObj, clsTextUrl)) {
                jstring urlStr = (jstring)env->GetObjectField(typeObj, fidUrl);
                if (urlStr) {
                    const char* urlChars = env->GetStringUTFChars(urlStr, nullptr);
                    if (urlChars) {
                        uint64_t emoji_id = 0;
                        if (parse_emoji_id_from_url(urlChars, emoji_id)) {
                            jobject customEmojiObj = env->NewObject(clsCustomEmoji, ctorCustomEmoji, (jlong)emoji_id);
                            if (customEmojiObj) {
                                env->SetObjectField(entity, fidEntityType, customEmojiObj);
                                jint curLen = env->GetIntField(entity, fidEntityLength);
                                if (curLen <= 0) {
                                    env->SetIntField(entity, fidEntityLength, 2);
                                }
                                {
                                    std::lock_guard<std::mutex> lock(s_requested_mutex);
                                    if (s_requested_emojis.insert(emoji_id).second) {
                                        ids_to_request.push_back(emoji_id);
                                    }
                                }
                                modified = true;
                            }
                        }
                        env->ReleaseStringUTFChars(urlStr, urlChars);
                    }
                }
            }
        }
    }
    
    if (jstrText) {
        const char* textChars = env->GetStringUTFChars(jstrText, nullptr);
        if (textChars && strstr(textChars, "tg://emoji?id=")) {
            std::string raw(textChars);
            std::string cleaned;
            std::vector<EmojiMatch> matches;
            if (transform_custom_emojis(raw, cleaned, matches) && !matches.empty()) {
                jstring newTextJStr = env->NewStringUTF(cleaned.c_str());
                if (newTextJStr) {
                    env->SetObjectField(fmtText, fidText, newTextJStr);
                }
                
                jsize oldCount = jarrEntities ? env->GetArrayLength(jarrEntities) : 0;
                jsize newCount = oldCount + (jsize)matches.size();
                jobjectArray newEntitiesArr = env->NewObjectArray(newCount, clsTextEntity, nullptr);
                
                if (newEntitiesArr) {
                    for (jsize i = 0; i < oldCount; ++i) {
                        jobject ent = env->GetObjectArrayElement(jarrEntities, i);
                        env->SetObjectArrayElement(newEntitiesArr, i, ent);
                    }
                    
                    for (size_t i = 0; i < matches.size(); ++i) {
                        const auto& m = matches[i];
                        jobject customEmojiObj = env->NewObject(clsCustomEmoji, ctorCustomEmoji, (jlong)m.emoji_id);
                        jobject newEnt = env->NewObject(clsTextEntity, ctorTextEntity, (jint)m.utf16_offset, (jint)m.utf16_length, customEmojiObj);
                        env->SetObjectArrayElement(newEntitiesArr, oldCount + (jsize)i, newEnt);
                        {
                            std::lock_guard<std::mutex> lock(s_requested_mutex);
                            if (s_requested_emojis.insert(m.emoji_id).second) {
                                ids_to_request.push_back(m.emoji_id);
                            }
                        }
                    }
                    
                    env->SetObjectField(fmtText, fidEntities, newEntitiesArr);
                    modified = true;
                }
            }
            env->ReleaseStringUTFChars(jstrText, textChars);
        } else if (textChars) {
            env->ReleaseStringUTFChars(jstrText, textChars);
        }
    }
    
    if (modified && !ids_to_request.empty()) {
        async_request_custom_emoji_stickers(ids_to_request);
        char log_buf[256];
        snprintf(log_buf, sizeof(log_buf), "Transformed %zu custom emoji(s)", ids_to_request.size());
        if (g_api) g_api->log_info(TAG, log_buf);
    }
}

static void before_message_project(HookParam* param) {
    if (!param || !param->args || param->args_count < 1) return;
    void* msg_handle = param->args[0];
    if (!msg_handle) return;
    
    JNIEnv* env = get_jni_env();
    if (!env) return;
    
    jclass clsHandleScope = find_app_class(env, "com/mandagram/plugin/bridge/ObjectHandleScope");
    if (!clsHandleScope) {
        env->ExceptionClear();
        return;
    }
    
    jmethodID midGetObject = env->GetStaticMethodID(clsHandleScope, "getObject", "(J)Ljava/lang/Object;");
    if (!midGetObject) {
        env->ExceptionClear();
        return;
    }
    
    jobject msg = env->CallStaticObjectMethod(clsHandleScope, midGetObject, (jlong)msg_handle);
    if (!msg) {
        env->ExceptionClear();
        return;
    }
    
    jclass clsMsg = env->GetObjectClass(msg);
    jfieldID fidContent = env->GetFieldID(clsMsg, "content", "Lit/tdlight/jni/TdApi$MessageContent;");
    if (!fidContent) {
        env->ExceptionClear();
        return;
    }
    
    jobject content = env->GetObjectField(msg, fidContent);
    if (!content) {
        env->ExceptionClear();
        return;
    }
    
    jclass clsContent = env->GetObjectClass(content);
    jfieldID fidFmtText = env->GetFieldID(clsContent, "text", "Lit/tdlight/jni/TdApi$FormattedText;");
    if (!fidFmtText) {
        env->ExceptionClear();
        fidFmtText = env->GetFieldID(clsContent, "caption", "Lit/tdlight/jni/TdApi$FormattedText;");
    }
    if (!fidFmtText) {
        env->ExceptionClear();
        return;
    }
    
    jobject fmtText = env->GetObjectField(content, fidFmtText);
    if (!fmtText) {
        env->ExceptionClear();
        return;
    }
    
    process_formatted_text(env, fmtText);
}

static thread_local char s_send_buffer[8192];

static const char* before_send_message(int64_t chat_id, const char* text, uintptr_t user_data) {
    if (!text || !strstr(text, "tg://emoji?id=")) {
        return text;
    }
    
    std::string raw(text);
    std::string out;
    size_t pos = 0;
    size_t last_pos = 0;
    bool altered = false;
    
    while ((pos = raw.find("tg://emoji?id=", pos)) != std::string::npos) {
        bool has_opening_paren = (pos > 0 && raw[pos - 1] == '(');
        if (has_opening_paren && pos > 1 && raw[pos - 2] == ']') {
            pos += 14;
            continue;
        }
        
        size_t id_start = pos + 14;
        char* endptr = nullptr;
        strtoull(&raw[id_start], &endptr, 10);
        if (endptr == &raw[id_start]) {
            pos = id_start;
            continue;
        }
        size_t url_end = (endptr - raw.data());
        if (has_opening_paren && url_end < raw.size() && raw[url_end] == ')') {
            url_end += 1;
        }
        
        size_t pattern_start = has_opening_paren ? (pos - 1) : pos;
        size_t emoji_start = find_emoji_cluster_start(raw, pattern_start);
        
        std::string emoji_str;
        size_t chunk_end = pattern_start;
        if (emoji_start >= last_pos && emoji_start < pattern_start) {
            chunk_end = emoji_start;
            emoji_str = raw.substr(emoji_start, pattern_start - emoji_start);
        } else {
            emoji_str = "🌟";
        }
        
        out += raw.substr(last_pos, chunk_end - last_pos);
        out += "[";
        out += emoji_str;
        out += "](";
        out += raw.substr(pos, (endptr - raw.data()) - pos);
        out += ")";
        
        last_pos = url_end;
        pos = url_end;
        altered = true;
    }
    
    if (altered) {
        if (last_pos < raw.length()) {
            out += raw.substr(last_pos);
        }
        snprintf(s_send_buffer, sizeof(s_send_buffer), "%s", out.c_str());
        return s_send_buffer;
    }
    
    return text;
}

extern "C" PLUGIN_EXPORT bool mandagram_plugin_init(const PluginApi* api) {
    if (!api) return false;
    g_api = api;
    
    g_api->log_info(TAG, "Initializing exmoji v1.0.0...");
    
    JNIEnv* env = get_jni_env();
    if (env) {
        jclass clsThread = env->FindClass("java/lang/Thread");
        if (clsThread) {
            jmethodID midCurrentThread = env->GetStaticMethodID(clsThread, "currentThread", "()Ljava/lang/Thread;");
            jobject curThread = env->CallStaticObjectMethod(clsThread, midCurrentThread);
            jmethodID midGetContextClassLoader = env->GetMethodID(clsThread, "getContextClassLoader", "()Ljava/lang/ClassLoader;");
            jobject classLoader = env->CallObjectMethod(curThread, midGetContextClassLoader);
            if (classLoader) {
                std::lock_guard<std::mutex> lock(g_loader_mutex);
                if (g_class_loader) {
                    env->DeleteGlobalRef(g_class_loader);
                }
                g_class_loader = env->NewGlobalRef(classLoader);
                g_api->log_info(TAG, "App ClassLoader captured successfully.");
            }
        }
    }
    
    int32_t hook_res = g_api->register_hook(
        "com.mandagram.network.engine.MessageItemProjector",
        "project",
        "(Lit/tdlight/jni/TdApi$Message;)Lcom/mandagram/models/MessageItem;",
        before_message_project,
        nullptr
    );
    
    if (hook_res == 0) {
        g_api->log_info(TAG, "Successfully hooked MessageItemProjector.project");
    } else {
        g_api->log_error(TAG, "Failed to hook MessageItemProjector.project");
    }
    
    int32_t send_hook_res = g_api->register_message_send_hook(before_send_message, nullptr, 0);
    if (send_hook_res == 0) {
        g_api->log_info(TAG, "Successfully registered message send hook");
    } else {
        g_api->log_error(TAG, "Failed to register message send hook");
    }
    
    g_api->log_info(TAG, "exmoji initialized successfully.");
    return true;
}
