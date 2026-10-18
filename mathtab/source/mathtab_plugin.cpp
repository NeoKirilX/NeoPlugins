#include "mandagram_plugin.h"

#include <string>
#include <vector>
#include <mutex>
#include <cmath>
#include <cctype>
#include <sstream>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <algorithm>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <jni.h>

static const char* TAG = "MathTab";

static const PluginMetadata s_metadata = {
    "MathTab",
    "1.2.1",
    "@NeoKirilX & @NeoPlugins",
    "Вычисляет математику, уравнения, цепочки (2+2=4-3=1), координаты и векторы (dist, mid, hypot, len, dot). По Tab вставляется =ответ, по Alt+Tab вставляется замена примера на ответ. Прозрачная подсказка и предпросмотр по Alt."
};

extern "C" PLUGIN_EXPORT const PluginMetadata* mandagram_plugin_metadata(void) {
    return &s_metadata;
}

static const PluginApi* g_api = nullptr;
static jobject g_class_loader = nullptr;
static std::mutex g_loader_mutex;

static jobject g_active_draft = nullptr;
static jobject g_active_on_change = nullptr;
static std::mutex g_state_mutex;
static std::atomic<bool> g_alt_pressed{false};

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

static JNIEnv* get_jni_env() {
    JavaVM* jvm = get_jvm();
    if (!jvm) return nullptr;
    JNIEnv* env = nullptr;
    jint res = jvm->GetEnv((void**)&env, JNI_VERSION_1_8);
    if (res == JNI_EDETACHED) {
        if (jvm->AttachCurrentThread((void**)&env, nullptr) == 0) {
            return env;
        }
        return nullptr;
    }
    if (res == JNI_OK) {
        return env;
    }
    return nullptr;
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

struct JniCache {
    std::atomic<bool> initialized{false};
    std::mutex mtx;

    jclass cls_builder = nullptr;
    jmethodID mid_builder_empty = nullptr;
    jmethodID mid_builder_copy = nullptr;
    jmethodID mid_push_style = nullptr;
    jmethodID mid_append_str = nullptr;
    jmethodID mid_append_range = nullptr;
    jmethodID mid_pop = nullptr;
    jmethodID mid_to_annotated = nullptr;

    jclass cls_mapping = nullptr;
    jmethodID mid_mapping_init = nullptr;

    jclass cls_transformed = nullptr;
    jmethodID mid_trans_init = nullptr;

    jclass cls_scope = nullptr;
    jmethodID mid_to_handle = nullptr;
    jmethodID mid_get_object = nullptr;

    jclass cls_annotated = nullptr;
    jmethodID mid_annotated_get_text = nullptr;

    jobject span_style = nullptr;

    jclass cls_key_event = nullptr;
    jmethodID mid_key_get_id = nullptr;
    jmethodID mid_key_get_code = nullptr;
    jmethodID mid_key_is_shift = nullptr;
    jmethodID mid_key_is_alt = nullptr;
    jmethodID mid_key_consume = nullptr;

    jclass cls_tfv = nullptr;
    jmethodID mid_tfv_get_text = nullptr;
    jmethodID mid_tfv_get_selection = nullptr;
    jmethodID mid_tfv_copy = nullptr;

    jclass cls_draft = nullptr;
    jmethodID mid_draft_get_value = nullptr;

    jclass cls_func1 = nullptr;
    jmethodID mid_func1_invoke = nullptr;
};
static JniCache g_cache;

static bool ensure_jni_cache(JNIEnv* env) {
    if (g_cache.initialized.load(std::memory_order_acquire)) return true;
    std::lock_guard<std::mutex> lock(g_cache.mtx);
    if (g_cache.initialized.load(std::memory_order_relaxed)) return true;

    jclass clsScope = find_app_class(env, "com/mandagram/plugin/bridge/ObjectHandleScope");
    if (!clsScope) return false;
    g_cache.cls_scope = (jclass)env->NewGlobalRef(clsScope);
    g_cache.mid_to_handle = env->GetStaticMethodID(clsScope, "toHandle", "(Ljava/lang/Object;)J");
    g_cache.mid_get_object = env->GetStaticMethodID(clsScope, "getObject", "(J)Ljava/lang/Object;");

    jclass clsBuilder = find_app_class(env, "androidx/compose/ui/text/AnnotatedString$Builder");
    if (!clsBuilder) return false;
    g_cache.cls_builder = (jclass)env->NewGlobalRef(clsBuilder);
    g_cache.mid_builder_empty = env->GetMethodID(clsBuilder, "<init>", "()V");
    g_cache.mid_builder_copy = env->GetMethodID(clsBuilder, "<init>", "(Landroidx/compose/ui/text/AnnotatedString;)V");
    g_cache.mid_push_style = env->GetMethodID(clsBuilder, "pushStyle", "(Landroidx/compose/ui/text/SpanStyle;)I");
    g_cache.mid_append_str = env->GetMethodID(clsBuilder, "append", "(Ljava/lang/String;)V");
    g_cache.mid_append_range = env->GetMethodID(clsBuilder, "append", "(Landroidx/compose/ui/text/AnnotatedString;II)V");
    g_cache.mid_pop = env->GetMethodID(clsBuilder, "pop", "()V");
    g_cache.mid_to_annotated = env->GetMethodID(clsBuilder, "toAnnotatedString", "()Landroidx/compose/ui/text/AnnotatedString;");

    jclass clsMapping = find_app_class(env, "com/mandagram/plugin/mathtab/MathTabOffsetMapping");
    if (!clsMapping) return false;
    g_cache.cls_mapping = (jclass)env->NewGlobalRef(clsMapping);
    g_cache.mid_mapping_init = env->GetMethodID(clsMapping, "<init>", "(II)V");

    jclass clsTransformed = find_app_class(env, "androidx/compose/ui/text/input/TransformedText");
    if (!clsTransformed) return false;
    g_cache.cls_transformed = (jclass)env->NewGlobalRef(clsTransformed);
    g_cache.mid_trans_init = env->GetMethodID(clsTransformed, "<init>", "(Landroidx/compose/ui/text/AnnotatedString;Landroidx/compose/ui/text/input/OffsetMapping;)V");

    jclass clsAnnotated = find_app_class(env, "androidx/compose/ui/text/AnnotatedString");
    if (!clsAnnotated) return false;
    g_cache.cls_annotated = (jclass)env->NewGlobalRef(clsAnnotated);
    g_cache.mid_annotated_get_text = env->GetMethodID(clsAnnotated, "getText", "()Ljava/lang/String;");

    jclass clsColorKt = find_app_class(env, "androidx/compose/ui/graphics/ColorKt");
    if (!clsColorKt) return false;
    jmethodID midColor = env->GetStaticMethodID(clsColorKt, "Color", "(I)J");
    if (!midColor) return false;
    jlong ghostColor = env->CallStaticLongMethod(clsColorKt, midColor, (jint)0x66888888);

    jclass clsSpanStyle = find_app_class(env, "androidx/compose/ui/text/SpanStyle");
    if (!clsSpanStyle) return false;
    jmethodID midSpanInit = env->GetMethodID(
        clsSpanStyle,
        "<init>",
        "(JJLandroidx/compose/ui/text/font/FontWeight;Landroidx/compose/ui/text/font/FontStyle;Landroidx/compose/ui/text/font/FontSynthesis;Landroidx/compose/ui/text/font/FontFamily;Ljava/lang/String;JLandroidx/compose/ui/text/style/BaselineShift;Landroidx/compose/ui/text/style/TextGeometricTransform;Landroidx/compose/ui/text/intl/LocaleList;JLandroidx/compose/ui/text/style/TextDecoration;Landroidx/compose/ui/graphics/Shadow;ILkotlin/jvm/internal/DefaultConstructorMarker;)V"
    );
    if (!midSpanInit) return false;
    jobject spanObj = env->NewObject(clsSpanStyle, midSpanInit, ghostColor, 0L, nullptr, nullptr, nullptr, nullptr, nullptr, 0L, nullptr, nullptr, nullptr, 0L, nullptr, nullptr, 0x7FFE, nullptr);
    if (!spanObj) return false;
    g_cache.span_style = env->NewGlobalRef(spanObj);

    jclass clsKeyEvent = env->FindClass("java/awt/event/KeyEvent");
    if (clsKeyEvent) {
        g_cache.cls_key_event = (jclass)env->NewGlobalRef(clsKeyEvent);
        g_cache.mid_key_get_id = env->GetMethodID(clsKeyEvent, "getID", "()I");
        g_cache.mid_key_get_code = env->GetMethodID(clsKeyEvent, "getKeyCode", "()I");
        g_cache.mid_key_is_shift = env->GetMethodID(clsKeyEvent, "isShiftDown", "()Z");
        g_cache.mid_key_is_alt = env->GetMethodID(clsKeyEvent, "isAltDown", "()Z");
        g_cache.mid_key_consume = env->GetMethodID(clsKeyEvent, "consume", "()V");
    }

    jclass clsTfv = find_app_class(env, "androidx/compose/ui/text/input/TextFieldValue");
    if (clsTfv) {
        g_cache.cls_tfv = (jclass)env->NewGlobalRef(clsTfv);
        g_cache.mid_tfv_get_text = env->GetMethodID(clsTfv, "getText", "()Ljava/lang/String;");
        g_cache.mid_tfv_get_selection = env->GetMethodID(clsTfv, "getSelection-d9O1mEE", "()J");
        g_cache.mid_tfv_copy = env->GetStaticMethodID(
            clsTfv,
            "copy-3r_uNRQ$default",
            "(Landroidx/compose/ui/text/input/TextFieldValue;Ljava/lang/String;JLandroidx/compose/ui/text/TextRange;ILjava/lang/Object;)Landroidx/compose/ui/text/input/TextFieldValue;"
        );
    }

    jclass clsDraft = find_app_class(env, "com/mandagram/ui/chat/ComposerDraft");
    if (clsDraft) {
        g_cache.cls_draft = (jclass)env->NewGlobalRef(clsDraft);
        g_cache.mid_draft_get_value = env->GetMethodID(clsDraft, "getValue", "()Landroidx/compose/ui/text/input/TextFieldValue;");
    }

    jclass clsFunc1 = find_app_class(env, "kotlin/jvm/functions/Function1");
    if (clsFunc1) {
        g_cache.cls_func1 = (jclass)env->NewGlobalRef(clsFunc1);
        g_cache.mid_func1_invoke = env->GetMethodID(clsFunc1, "invoke", "(Ljava/lang/Object;)Ljava/lang/Object;");
    }

    g_cache.initialized.store(true, std::memory_order_release);
    return true;
}

static jobject get_object_from_handle(JNIEnv* env, void* handle) {
    if (!env || !handle) return nullptr;
    if (g_cache.initialized.load(std::memory_order_relaxed) && g_cache.cls_scope && g_cache.mid_get_object) {
        return env->CallStaticObjectMethod(g_cache.cls_scope, g_cache.mid_get_object, (jlong)handle);
    }
    jclass clsHandleScope = find_app_class(env, "com/mandagram/plugin/bridge/ObjectHandleScope");
    if (!clsHandleScope) return nullptr;
    jmethodID midGetObject = env->GetStaticMethodID(clsHandleScope, "getObject", "(J)Ljava/lang/Object;");
    if (!midGetObject) {
        env->ExceptionClear();
        return nullptr;
    }
    jobject obj = env->CallStaticObjectMethod(clsHandleScope, midGetObject, (jlong)handle);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return nullptr;
    }
    return obj;
}

static int utf8_to_utf16_offset(const std::string& s, int u8_offset) {
    if (u8_offset <= 0) return 0;
    if (u8_offset > (int)s.length()) u8_offset = (int)s.length();
    int u16 = 0;
    for (int i = 0; i < u8_offset; ) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { i += 1; u16 += 1; }
        else if ((c & 0xE0) == 0xC0) { i += 2; u16 += 1; }
        else if ((c & 0xF0) == 0xE0) { i += 3; u16 += 1; }
        else if ((c & 0xF8) == 0xF0) { i += 4; u16 += 2; }
        else { i += 1; u16 += 1; }
    }
    return u16;
}

static int utf16_to_utf8_offset(const std::string& s, int u16_offset) {
    if (u16_offset <= 0) return 0;
    int u16 = 0;
    int i = 0;
    while (i < (int)s.length() && u16 < u16_offset) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { i += 1; u16 += 1; }
        else if ((c & 0xE0) == 0xC0) { i += 2; u16 += 1; }
        else if ((c & 0xF0) == 0xE0) { i += 3; u16 += 1; }
        else if ((c & 0xF8) == 0xF0) { i += 4; u16 += 2; }
        else { i += 1; u16 += 1; }
    }
    return i;
}

struct Value {
    bool is_vec = false;
    double num = 0.0;
    std::vector<double> vec;

    Value() : is_vec(false), num(0.0) {}
    Value(double v) : is_vec(false), num(v) {}
    Value(const std::vector<double>& v) : is_vec(true), num(0.0), vec(v) {}
};

static std::string format_number(double val) {
    if (std::abs(val - std::round(val)) < 1e-9 && std::abs(val) < 1e15) {
        return std::to_string((long long)std::round(val));
    }
    std::ostringstream oss;
    oss.precision(8);
    oss << val;
    std::string s = oss.str();
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s;
}

static std::string format_value(const Value& v) {
    if (!v.is_vec) return format_number(v.num);
    std::string s = "(";
    for (size_t i = 0; i < v.vec.size(); ++i) {
        if (i > 0) s += ", ";
        s += format_number(v.vec[i]);
    }
    s += ")";
    return s;
}

enum MathTokenType {
    TOK_END,
    TOK_NUM,
    TOK_PLUS,
    TOK_MINUS,
    TOK_MUL,
    TOK_DIV,
    TOK_POW,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_COMMA,
    TOK_VAR,
    TOK_FUNC
};

enum FuncType {
    FN_NONE,
    FN_SQRT,
    FN_CBRT,
    FN_ABS,
    FN_SIN,
    FN_COS,
    FN_TAN,
    FN_LN,
    FN_LOG,
    FN_LOG2,
    FN_EXP,
    FN_ROUND,
    FN_FLOOR,
    FN_CEIL,
    FN_DIST,
    FN_HYPOT,
    FN_MID,
    FN_LEN,
    FN_DOT
};

struct MathToken {
    MathTokenType kind;
    double value;
    FuncType func;
    MathToken() : kind(TOK_END), value(0.0), func(FN_NONE) {}
    MathToken(MathTokenType k, double v) : kind(k), value(v), func(FN_NONE) {}
    MathToken(MathTokenType k, FuncType f) : kind(k), value(0.0), func(f) {}
};

class MathParser {
    std::string src;
    size_t pos;
    MathToken current;
    double var_val;
    int paren_depth;

    void advance() {
        while (pos < src.length() && isspace((unsigned char)src[pos])) pos++;
        if (pos >= src.length()) {
            current = MathToken(TOK_END, 0.0);
            return;
        }
        char c = src[pos];
        if (c == ',') {
            pos++;
            current = MathToken(TOK_COMMA, 0.0);
            return;
        }
        if (isdigit((unsigned char)c) || c == '.') {
            size_t start = pos;
            bool has_sep = (c == '.');
            pos++;
            while (pos < src.length()) {
                char ch = src[pos];
                if (isdigit((unsigned char)ch)) {
                    pos++;
                } else if (ch == '.') {
                    if (has_sep) break;
                    has_sep = true;
                    pos++;
                } else if (ch == ',') {
                    if (paren_depth > 0) break;
                    if (has_sep) break;
                    if (pos + 1 < src.length() && isdigit((unsigned char)src[pos + 1])) {
                        has_sep = true;
                        pos++;
                    } else {
                        break;
                    }
                } else {
                    break;
                }
            }
            std::string num_str = src.substr(start, pos - start);
            for (char& ch : num_str) if (ch == ',') ch = '.';
            try {
                double val = std::stod(num_str);
                if (pos < src.length() && src[pos] == '%') {
                    val /= 100.0;
                    pos++;
                }
                current = MathToken(TOK_NUM, val);
            } catch (...) {
                current = MathToken(TOK_END, 0.0);
            }
            return;
        }

        if (isalpha((unsigned char)c) || (unsigned char)c >= 0x80) {
            size_t start = pos;
            while (pos < src.length() && (isalpha((unsigned char)src[pos]) || (unsigned char)src[pos] >= 0x80)) {
                pos++;
            }
            std::string ident = src.substr(start, pos - start);
            for (char& ch : ident) ch = (char)tolower((unsigned char)ch);

            if (ident == "pi" || ident == "\xd0\xbf\xd0\xb8") {
                current = MathToken(TOK_NUM, 3.14159265358979323846);
                return;
            }
            if (ident == "e") {
                current = MathToken(TOK_NUM, 2.71828182845904523536);
                return;
            }
            if (ident == "x" || ident == "y" || ident == "z" || ident == "\xd1\x85" || ident == "\xd1\x83") {
                current = MathToken(TOK_VAR, var_val);
                return;
            }

            FuncType fn = FN_NONE;
            if (ident == "sqrt") fn = FN_SQRT;
            else if (ident == "cbrt") fn = FN_CBRT;
            else if (ident == "abs") fn = FN_ABS;
            else if (ident == "sin") fn = FN_SIN;
            else if (ident == "cos") fn = FN_COS;
            else if (ident == "tan") fn = FN_TAN;
            else if (ident == "ln") fn = FN_LN;
            else if (ident == "log" || ident == "log10") fn = FN_LOG;
            else if (ident == "log2") fn = FN_LOG2;
            else if (ident == "exp") fn = FN_EXP;
            else if (ident == "round") fn = FN_ROUND;
            else if (ident == "floor") fn = FN_FLOOR;
            else if (ident == "ceil") fn = FN_CEIL;
            else if (ident == "dist" || ident == "distance") fn = FN_DIST;
            else if (ident == "hypot") fn = FN_HYPOT;
            else if (ident == "mid" || ident == "midpoint") fn = FN_MID;
            else if (ident == "len" || ident == "length" || ident == "norm" || ident == "mag") fn = FN_LEN;
            else if (ident == "dot") fn = FN_DOT;

            if (fn != FN_NONE) {
                current = MathToken(TOK_FUNC, fn);
                return;
            }

            current = MathToken(TOK_END, 0.0);
            return;
        }

        pos++;
        switch (c) {
            case '+': current = MathToken(TOK_PLUS, 0.0); break;
            case '-': current = MathToken(TOK_MINUS, 0.0); break;
            case '*': 
            case 'x':
            case 'X': current = MathToken(TOK_MUL, 0.0); break;
            case '/': 
            case ':':
            case '\\': current = MathToken(TOK_DIV, 0.0); break;
            case '^': current = MathToken(TOK_POW, 0.0); break;
            case '(': paren_depth++; current = MathToken(TOK_LPAREN, 0.0); break;
            case ')': if (paren_depth > 0) paren_depth--; current = MathToken(TOK_RPAREN, 0.0); break;
            case '[': paren_depth++; current = MathToken(TOK_LBRACKET, 0.0); break;
            case ']': if (paren_depth > 0) paren_depth--; current = MathToken(TOK_RBRACKET, 0.0); break;
            default:  current = MathToken(TOK_END, 0.0); break;
        }
    }

    Value op_add(const Value& a, const Value& b) {
        if (!a.is_vec && !b.is_vec) return Value(a.num + b.num);
        if (a.is_vec && b.is_vec) {
            size_t n = std::min(a.vec.size(), b.vec.size());
            std::vector<double> r(n);
            for (size_t i = 0; i < n; ++i) r[i] = a.vec[i] + b.vec[i];
            return Value(r);
        }
        if (a.is_vec) {
            std::vector<double> r(a.vec.size());
            for (size_t i = 0; i < a.vec.size(); ++i) r[i] = a.vec[i] + b.num;
            return Value(r);
        }
        std::vector<double> r(b.vec.size());
        for (size_t i = 0; i < b.vec.size(); ++i) r[i] = a.num + b.vec[i];
        return Value(r);
    }

    Value op_sub(const Value& a, const Value& b) {
        if (!a.is_vec && !b.is_vec) return Value(a.num - b.num);
        if (a.is_vec && b.is_vec) {
            size_t n = std::min(a.vec.size(), b.vec.size());
            std::vector<double> r(n);
            for (size_t i = 0; i < n; ++i) r[i] = a.vec[i] - b.vec[i];
            return Value(r);
        }
        if (a.is_vec) {
            std::vector<double> r(a.vec.size());
            for (size_t i = 0; i < a.vec.size(); ++i) r[i] = a.vec[i] - b.num;
            return Value(r);
        }
        std::vector<double> r(b.vec.size());
        for (size_t i = 0; i < b.vec.size(); ++i) r[i] = a.num - b.vec[i];
        return Value(r);
    }

    Value op_mul(const Value& a, const Value& b) {
        if (!a.is_vec && !b.is_vec) return Value(a.num * b.num);
        if (a.is_vec && b.is_vec) {
            size_t n = std::min(a.vec.size(), b.vec.size());
            double sum = 0.0;
            for (size_t i = 0; i < n; ++i) sum += a.vec[i] * b.vec[i];
            return Value(sum);
        }
        if (a.is_vec) {
            std::vector<double> r(a.vec.size());
            for (size_t i = 0; i < a.vec.size(); ++i) r[i] = a.vec[i] * b.num;
            return Value(r);
        }
        std::vector<double> r(b.vec.size());
        for (size_t i = 0; i < b.vec.size(); ++i) r[i] = a.num * b.vec[i];
        return Value(r);
    }

    Value op_div(const Value& a, const Value& b) {
        if (!a.is_vec && !b.is_vec) {
            if (std::abs(b.num) < 1e-15) return Value(0.0);
            return Value(a.num / b.num);
        }
        if (a.is_vec && !b.is_vec) {
            if (std::abs(b.num) < 1e-15) return Value(a);
            std::vector<double> r(a.vec.size());
            for (size_t i = 0; i < a.vec.size(); ++i) r[i] = a.vec[i] / b.num;
            return Value(r);
        }
        return Value(0.0);
    }

    bool parse_factor(Value& out) {
        if (current.kind == TOK_PLUS) {
            advance();
            return parse_factor(out);
        }
        if (current.kind == TOK_MINUS) {
            advance();
            Value val;
            if (!parse_factor(val)) return false;
            if (val.is_vec) {
                for (double& d : val.vec) d = -d;
                out = val;
            } else {
                out = Value(-val.num);
            }
            return true;
        }
        if (current.kind == TOK_NUM || current.kind == TOK_VAR) {
            out = Value(current.value);
            advance();
            return true;
        }
        if (current.kind == TOK_FUNC) {
            FuncType fn = current.func;
            advance();
            std::vector<Value> args;
            if (current.kind == TOK_LPAREN) {
                advance();
                if (current.kind != TOK_RPAREN) {
                    while (true) {
                        Value a;
                        if (!parse_expr(a)) return false;
                        args.push_back(a);
                        if (current.kind == TOK_COMMA) {
                            advance();
                        } else {
                            break;
                        }
                    }
                }
                if (current.kind != TOK_RPAREN) return false;
                advance();
            } else {
                Value a;
                if (!parse_factor(a)) return false;
                args.push_back(a);
            }

            if (fn == FN_SQRT) {
                double a = args.empty() ? 0.0 : args[0].num;
                out = Value((a >= 0.0) ? std::sqrt(a) : 0.0);
            } else if (fn == FN_CBRT) {
                double a = args.empty() ? 0.0 : args[0].num;
                out = Value(std::cbrt(a));
            } else if (fn == FN_ABS) {
                double a = args.empty() ? 0.0 : args[0].num;
                out = Value(std::abs(a));
            } else if (fn == FN_SIN) {
                out = Value(std::sin(args.empty() ? 0.0 : args[0].num));
            } else if (fn == FN_COS) {
                out = Value(std::cos(args.empty() ? 0.0 : args[0].num));
            } else if (fn == FN_TAN) {
                out = Value(std::tan(args.empty() ? 0.0 : args[0].num));
            } else if (fn == FN_LN) {
                double a = args.empty() ? 0.0 : args[0].num;
                out = Value(a > 0.0 ? std::log(a) : 0.0);
            } else if (fn == FN_LOG) {
                double a = args.empty() ? 0.0 : args[0].num;
                out = Value(a > 0.0 ? std::log10(a) : 0.0);
            } else if (fn == FN_LOG2) {
                double a = args.empty() ? 0.0 : args[0].num;
                out = Value(a > 0.0 ? std::log2(a) : 0.0);
            } else if (fn == FN_EXP) {
                out = Value(std::exp(args.empty() ? 0.0 : args[0].num));
            } else if (fn == FN_ROUND) {
                out = Value(std::round(args.empty() ? 0.0 : args[0].num));
            } else if (fn == FN_FLOOR) {
                out = Value(std::floor(args.empty() ? 0.0 : args[0].num));
            } else if (fn == FN_CEIL) {
                out = Value(std::ceil(args.empty() ? 0.0 : args[0].num));
            } else if (fn == FN_DIST) {
                if (args.size() == 2 && args[0].is_vec && args[1].is_vec) {
                    double sum = 0.0;
                    size_t n = std::min(args[0].vec.size(), args[1].vec.size());
                    for (size_t i = 0; i < n; ++i) {
                        double d = args[0].vec[i] - args[1].vec[i];
                        sum += d * d;
                    }
                    out = Value(std::sqrt(sum));
                } else if (args.size() == 4) {
                    double dx = args[2].num - args[0].num;
                    double dy = args[3].num - args[1].num;
                    out = Value(std::sqrt(dx * dx + dy * dy));
                } else if (args.size() == 6) {
                    double dx = args[3].num - args[0].num;
                    double dy = args[4].num - args[1].num;
                    double dz = args[5].num - args[2].num;
                    out = Value(std::sqrt(dx * dx + dy * dy + dz * dz));
                } else {
                    return false;
                }
            } else if (fn == FN_HYPOT || fn == FN_LEN) {
                if (args.size() == 1 && args[0].is_vec) {
                    double sum = 0.0;
                    for (double d : args[0].vec) sum += d * d;
                    out = Value(std::sqrt(sum));
                } else if (!args.empty()) {
                    double sum = 0.0;
                    for (const auto& a : args) sum += a.num * a.num;
                    out = Value(std::sqrt(sum));
                } else {
                    return false;
                }
            } else if (fn == FN_MID) {
                if (args.size() == 2 && args[0].is_vec && args[1].is_vec) {
                    size_t n = std::min(args[0].vec.size(), args[1].vec.size());
                    std::vector<double> r(n);
                    for (size_t i = 0; i < n; ++i) r[i] = (args[0].vec[i] + args[1].vec[i]) / 2.0;
                    out = Value(r);
                } else if (args.size() == 4) {
                    out = Value(std::vector<double>{ (args[0].num + args[2].num)/2.0, (args[1].num + args[3].num)/2.0 });
                } else if (args.size() == 6) {
                    out = Value(std::vector<double>{ (args[0].num + args[3].num)/2.0, (args[1].num + args[4].num)/2.0, (args[2].num + args[5].num)/2.0 });
                } else {
                    return false;
                }
            } else if (fn == FN_DOT) {
                if (args.size() == 2 && args[0].is_vec && args[1].is_vec) {
                    out = op_mul(args[0], args[1]);
                } else if (args.size() == 4) {
                    out = Value(args[0].num * args[2].num + args[1].num * args[3].num);
                } else if (args.size() == 6) {
                    out = Value(args[0].num * args[3].num + args[1].num * args[4].num + args[2].num * args[5].num);
                } else {
                    return false;
                }
            }
            return true;
        }
        if (current.kind == TOK_LPAREN || current.kind == TOK_LBRACKET) {
            MathTokenType close_tok = (current.kind == TOK_LPAREN) ? TOK_RPAREN : TOK_RBRACKET;
            advance();
            std::vector<Value> list;
            while (true) {
                Value elem;
                if (!parse_expr(elem)) return false;
                list.push_back(elem);
                if (current.kind == TOK_COMMA) {
                    advance();
                } else {
                    break;
                }
            }
            if (current.kind != close_tok) return false;
            advance();
            if (list.size() == 1) {
                out = list[0];
            } else {
                std::vector<double> vec;
                for (const auto& elem : list) vec.push_back(elem.num);
                out = Value(vec);
            }
            return true;
        }
        return false;
    }

    bool parse_power(Value& out) {
        if (!parse_factor(out)) return false;
        if (current.kind == TOK_POW) {
            advance();
            Value right;
            if (!parse_power(right)) return false;
            if (!out.is_vec && !right.is_vec) {
                out = Value(std::pow(out.num, right.num));
            }
        }
        return true;
    }

    bool parse_term(Value& out) {
        if (!parse_power(out)) return false;
        while (current.kind == TOK_MUL || current.kind == TOK_DIV) {
            MathTokenType op = current.kind;
            advance();
            Value right;
            if (!parse_power(right)) return false;
            if (op == TOK_MUL) {
                out = op_mul(out, right);
            } else {
                out = op_div(out, right);
            }
        }
        return true;
    }

    bool parse_expr(Value& out) {
        if (!parse_term(out)) return false;
        while (current.kind == TOK_PLUS || current.kind == TOK_MINUS) {
            MathTokenType op = current.kind;
            advance();
            Value right;
            if (!parse_term(right)) return false;
            if (op == TOK_PLUS) {
                out = op_add(out, right);
            } else {
                out = op_sub(out, right);
            }
        }
        return true;
    }

public:
    MathParser(const std::string& input, double v_val = 0.0) : src(input), pos(0), var_val(v_val), paren_depth(0) {
        advance();
    }

    bool evaluate(Value& result) {
        if (!parse_expr(result)) return false;
        return current.kind == TOK_END;
    }
};

static std::string normalize_implicit_mul(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.length(); ++i) {
        char c = s[i];
        if (i > 0 && (isdigit((unsigned char)s[i - 1]) || s[i - 1] == ')' || s[i - 1] == ']') &&
            (isalpha((unsigned char)c) || c == '(' || c == '[' || (unsigned char)c >= 0x80)) {
            out += '*';
        }
        out += c;
    }
    return out;
}

static bool is_math_keyword(const std::string& word) {
    if (word == "x" || word == "y" || word == "z" || word == "X" || word == "Y" || word == "Z") return true;
    if (word == "\xd1\x85" || word == "\xd1\x83" || word == "\xd0\xa5" || word == "\xd0\xa3") return true;
    if (word == "pi" || word == "PI" || word == "\xd0\xbf\xd0\xb8" || word == "\xd0\x9f\xd0\x98") return true;
    if (word == "e" || word == "E") return true;
    if (word == "sqrt" || word == "cbrt" || word == "abs") return true;
    if (word == "sin" || word == "cos" || word == "tan") return true;
    if (word == "ln" || word == "log" || word == "log2" || word == "log10") return true;
    if (word == "exp" || word == "round" || word == "floor" || word == "ceil") return true;
    if (word == "dist" || word == "distance" || word == "hypot") return true;
    if (word == "mid" || word == "midpoint" || word == "dot") return true;
    if (word == "len" || word == "length" || word == "norm" || word == "mag") return true;
    return false;
}

static inline bool is_math_delim(char c) {
    return c == '+' || c == '-' || c == '*' || c == '/' || c == ':' || c == '\\' ||
           c == '^' || c == '%' || c == '=' || c == '(' || c == ')' || c == '[' ||
           c == ']' || c == ',';
}

static bool is_already_solved(const std::string& candidate) {
    if (candidate.find("(x=") != std::string::npos || candidate.find("(y=") != std::string::npos ||
        candidate.find("(z=") != std::string::npos || candidate.find("(\xd1\x85=") != std::string::npos ||
        candidate.find("(\xd1\x83=") != std::string::npos) {
        return true;
    }
    size_t last_eq = candidate.rfind('=');
    if (last_eq != std::string::npos) {
        std::string after_eq = candidate.substr(last_eq + 1);
        size_t first_non_ws = after_eq.find_first_not_of(" \t");
        if (first_non_ws != std::string::npos) {
            std::string sub = after_eq.substr(first_non_ws);
            bool has_var = (candidate.find("x") != std::string::npos || candidate.find("y") != std::string::npos ||
                            candidate.find("z") != std::string::npos || candidate.find("\xd1\x85") != std::string::npos ||
                            candidate.find("\xd1\x83") != std::string::npos);
            if (has_var) {
                return false;
            }
            bool has_op = false;
            for (char c : sub) {
                if (c == '+' || c == '-' || c == '*' || c == '/' || c == ':' || c == '\\' || c == '^' || c == '%') {
                    has_op = true;
                    break;
                }
            }
            if (!has_op) {
                if (sub.find("sqrt") != std::string::npos || sub.find("dist") != std::string::npos ||
                    sub.find("hypot") != std::string::npos || sub.find("mid") != std::string::npos ||
                    sub.find("sin") != std::string::npos || sub.find("cos") != std::string::npos ||
                    sub.find("dot") != std::string::npos || sub.find("len") != std::string::npos) {
                    has_op = true;
                }
            }
            if (!has_op) {
                return true;
            }
        }
    }
    return false;
}

static bool solve_equation(const std::string& input, std::string& out_var, double& out_root) {
    size_t eq = input.find('=');
    if (eq == std::string::npos) return false;
    std::string lhs = normalize_implicit_mul(input.substr(0, eq));
    std::string rhs = normalize_implicit_mul(input.substr(eq + 1));
    while (!lhs.empty() && isspace((unsigned char)lhs.back())) lhs.pop_back();
    while (!rhs.empty() && isspace((unsigned char)rhs.front())) rhs.erase(rhs.begin());
    if (lhs.empty() || rhs.empty()) return false;

    std::string var_name;
    int var_count = 0;
    for (const char* v : {"x", "y", "z", "\xd1\x85", "\xd1\x83"}) {
        if (lhs.find(v) != std::string::npos || rhs.find(v) != std::string::npos) {
            if (var_name.empty()) var_name = v;
            var_count++;
        }
    }
    if (var_count != 1) return false;

    auto eval_diff = [&](double val) -> double {
        Value l, r;
        MathParser pl(lhs, val);
        if (!pl.evaluate(l)) return 1e18;
        MathParser pr(rhs, val);
        if (!pr.evaluate(r)) return 1e18;
        return l.num - r.num;
    };

    double f0 = eval_diff(0.0);
    double f1 = eval_diff(1.0);
    if (std::abs(f0) < 1e15 && std::abs(f1) < 1e15 && std::abs(f0 - f1) > 1e-12) {
        double root = f0 / (f0 - f1);
        if (std::abs(eval_diff(root)) < 1e-5) {
            out_var = var_name;
            out_root = root;
            return true;
        }
    }

    double x0 = 1.0, x1 = 2.0;
    double fx0 = eval_diff(x0);
    for (int i = 0; i < 15; i++) {
        double fx1 = eval_diff(x1);
        if (std::abs(fx1 - fx0) < 1e-14) break;
        double x2 = x1 - fx1 * (x1 - x0) / (fx1 - fx0);
        x0 = x1;
        fx0 = fx1;
        x1 = x2;
        if (std::abs(eval_diff(x1)) < 1e-5) {
            out_var = var_name;
            out_root = x1;
            return true;
        }
    }
    return false;
}

static bool evaluate_candidate(const std::string& candidate, std::string& out_solution, bool& is_equation) {
    if (is_already_solved(candidate)) return false;

    std::string var_name;
    double root = 0.0;
    if (solve_equation(candidate, var_name, root)) {
        if (!std::isnan(root) && !std::isinf(root)) {
            out_solution = var_name + "=" + format_number(root);
            is_equation = true;
            return true;
        }
    }

    std::string expr = candidate;
    size_t last_eq = expr.rfind('=');
    if (last_eq != std::string::npos) {
        std::string after = expr.substr(last_eq + 1);
        size_t non_ws = after.find_first_not_of(" \t");
        if (non_ws == std::string::npos) {
            expr = expr.substr(0, last_eq);
        } else {
            expr = after.substr(non_ws);
        }
    }

    while (!expr.empty() && isspace((unsigned char)expr.back())) expr.pop_back();
    if (expr.empty()) return false;

    bool has_op = false;
    for (char c : expr) {
        if (c == '+' || c == '-' || c == '*' || c == 'x' || c == 'X' || c == '/' || c == ':' || c == '\\' || c == '^' || c == '%') {
            has_op = true;
            break;
        }
    }
    if (!has_op) {
        if (expr.find("sqrt") != std::string::npos || expr.find("cbrt") != std::string::npos ||
            expr.find("sin") != std::string::npos || expr.find("cos") != std::string::npos ||
            expr.find("abs") != std::string::npos || expr.find("pi") != std::string::npos ||
            expr.find("\xd0\xbf\xd0\xb8") != std::string::npos ||
            expr.find("dist") != std::string::npos || expr.find("hypot") != std::string::npos ||
            expr.find("mid") != std::string::npos || expr.find("len") != std::string::npos ||
            expr.find("dot") != std::string::npos ||
            expr.find("log") != std::string::npos || expr.find("ln") != std::string::npos) {
            has_op = true;
        }
    }
    if (!has_op) return false;

    std::string norm = normalize_implicit_mul(expr);
    Value result;
    MathParser parser(norm);
    if (!parser.evaluate(result)) return false;
    if (!result.is_vec && (std::isnan(result.num) || std::isinf(result.num))) return false;

    out_solution = format_value(result);
    is_equation = false;
    return true;
}

static bool evaluate_candidate_cached(const std::string& candidate, std::string& out_solution, bool& is_equation) {
    static std::string s_last_candidate;
    static std::string s_last_solution;
    static bool s_last_is_equation = false;
    static bool s_last_eval_ok = false;
    static std::mutex s_eval_mutex;

    std::lock_guard<std::mutex> lock(s_eval_mutex);
    if (candidate == s_last_candidate) {
        if (s_last_eval_ok) {
            out_solution = s_last_solution;
            is_equation = s_last_is_equation;
        }
        return s_last_eval_ok;
    }

    s_last_candidate = candidate;
    s_last_eval_ok = evaluate_candidate(candidate, s_last_solution, s_last_is_equation);
    if (s_last_eval_ok) {
        out_solution = s_last_solution;
        is_equation = s_last_is_equation;
    }
    return s_last_eval_ok;
}

static bool find_math_candidate(const std::string& text, int cursor, int& out_start, std::string& out_candidate) {
    if (cursor <= 0 || cursor > (int)text.length()) return false;
    int end = cursor;
    while (end > 0 && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
        end--;
    }
    if (end == 0) return false;

    int i = end;
    bool has_math_char = false;
    int parens = 0;

    while (i > 0) {
        char c = text[i - 1];
        if (c == ')' || c == ']') {
            parens++;
            i--;
            has_math_char = true;
        } else if (c == '(' || c == '[') {
            parens--;
            i--;
            has_math_char = true;
            if (parens < 0) {
                i++;
                break;
            }
        } else if (isdigit((unsigned char)c) || is_math_delim(c) || c == '.') {
            has_math_char = true;
            i--;
        } else if (isalpha((unsigned char)c) || (unsigned char)c >= 0x80) {
            int word_end = i;
            while (i > 0 && (isalpha((unsigned char)text[i - 1]) || (unsigned char)text[i - 1] >= 0x80)) {
                i--;
            }
            std::string word = text.substr(i, word_end - i);
            std::string lower_word = word;
            for (char& ch : lower_word) ch = (char)tolower((unsigned char)ch);
            if (!is_math_keyword(lower_word)) {
                i = word_end;
                break;
            }
            has_math_char = true;
        } else if (c == ' ' || c == '\t') {
            int peek = i - 1;
            while (peek > 0 && (text[peek - 1] == ' ' || text[peek - 1] == '\t')) {
                peek--;
            }
            if (peek == 0) {
                break;
            }
            char prev_c = text[peek - 1];
            char next_c = text[i];
            bool prev_is_op = is_math_delim(prev_c);
            bool next_is_op = is_math_delim(next_c);

            if (prev_is_op || next_is_op) {
                i = peek;
            } else if (isdigit((unsigned char)prev_c) && (parens > 0)) {
                i = peek;
            } else if (isalpha((unsigned char)prev_c) || (unsigned char)prev_c >= 0x80) {
                int w_end = peek;
                int w_start = peek;
                while (w_start > 0 && (isalpha((unsigned char)text[w_start - 1]) || (unsigned char)text[w_start - 1] >= 0x80)) {
                    w_start--;
                }
                std::string w = text.substr(w_start, w_end - w_start);
                for (char& ch : w) ch = (char)tolower((unsigned char)ch);
                if (is_math_keyword(w)) {
                    i = w_start;
                } else {
                    break;
                }
            } else {
                break;
            }
        } else {
            break;
        }
    }

    while (i < end && (text[i] == ' ' || text[i] == '\t')) {
        i++;
    }
    if (i >= end || !has_math_char) return false;

    out_start = i;
    out_candidate = text.substr(i, end - i);
    return true;
}

static const unsigned char s_dispatcher_class_bytes[] = {
    0xca, 0xfe, 0xba, 0xbe, 0x00, 0x00, 0x00, 0x34, 0x00, 0x14, 0x0a, 0x00, 0x02, 0x00, 0x03, 0x07,
    0x00, 0x04, 0x0c, 0x00, 0x05, 0x00, 0x06, 0x01, 0x00, 0x10, 0x6a, 0x61, 0x76, 0x61, 0x2f, 0x6c,
    0x61, 0x6e, 0x67, 0x2f, 0x4f, 0x62, 0x6a, 0x65, 0x63, 0x74, 0x01, 0x00, 0x06, 0x3c, 0x69, 0x6e,
    0x69, 0x74, 0x3e, 0x01, 0x00, 0x03, 0x28, 0x29, 0x56, 0x0a, 0x00, 0x08, 0x00, 0x09, 0x07, 0x00,
    0x0a, 0x0c, 0x00, 0x0b, 0x00, 0x0c, 0x01, 0x00, 0x2e, 0x63, 0x6f, 0x6d, 0x2f, 0x6d, 0x61, 0x6e,
    0x64, 0x61, 0x67, 0x72, 0x61, 0x6d, 0x2f, 0x70, 0x6c, 0x75, 0x67, 0x69, 0x6e, 0x2f, 0x6d, 0x61,
    0x74, 0x68, 0x74, 0x61, 0x62, 0x2f, 0x4d, 0x61, 0x74, 0x68, 0x54, 0x61, 0x62, 0x44, 0x69, 0x73,
    0x70, 0x61, 0x74, 0x63, 0x68, 0x65, 0x72, 0x01, 0x00, 0x0a, 0x6f, 0x6e, 0x4b, 0x65, 0x79, 0x45,
    0x76, 0x65, 0x6e, 0x74, 0x01, 0x00, 0x1c, 0x28, 0x4c, 0x6a, 0x61, 0x76, 0x61, 0x2f, 0x61, 0x77,
    0x74, 0x2f, 0x65, 0x76, 0x65, 0x6e, 0x74, 0x2f, 0x4b, 0x65, 0x79, 0x45, 0x76, 0x65, 0x6e, 0x74,
    0x3b, 0x29, 0x5a, 0x07, 0x00, 0x0e, 0x01, 0x00, 0x1b, 0x6a, 0x61, 0x76, 0x61, 0x2f, 0x61, 0x77,
    0x74, 0x2f, 0x4b, 0x65, 0x79, 0x45, 0x76, 0x65, 0x6e, 0x74, 0x44, 0x69, 0x73, 0x70, 0x61, 0x74,
    0x63, 0x68, 0x65, 0x72, 0x01, 0x00, 0x04, 0x43, 0x6f, 0x64, 0x65, 0x01, 0x00, 0x0f, 0x4c, 0x69,
    0x6e, 0x65, 0x4e, 0x75, 0x6d, 0x62, 0x65, 0x72, 0x54, 0x61, 0x62, 0x6c, 0x65, 0x01, 0x00, 0x10,
    0x64, 0x69, 0x73, 0x70, 0x61, 0x74, 0x63, 0x68, 0x4b, 0x65, 0x79, 0x45, 0x76, 0x65, 0x6e, 0x74,
    0x01, 0x00, 0x0a, 0x53, 0x6f, 0x75, 0x72, 0x63, 0x65, 0x46, 0x69, 0x6c, 0x65, 0x01, 0x00, 0x16,
    0x4d, 0x61, 0x74, 0x68, 0x54, 0x61, 0x62, 0x44, 0x69, 0x73, 0x70, 0x61, 0x74, 0x63, 0x68, 0x65,
    0x72, 0x2e, 0x6a, 0x61, 0x76, 0x61, 0x00, 0x21, 0x00, 0x08, 0x00, 0x02, 0x00, 0x01, 0x00, 0x0d,
    0x00, 0x00, 0x00, 0x03, 0x00, 0x01, 0x00, 0x05, 0x00, 0x06, 0x00, 0x01, 0x00, 0x0f, 0x00, 0x00,
    0x00, 0x1d, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x05, 0x2a, 0xb7, 0x00, 0x01, 0xb1, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x10, 0x00, 0x00, 0x00, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03, 0x01,
    0x09, 0x00, 0x0b, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x01, 0x00, 0x11, 0x00, 0x0c, 0x00, 0x01, 0x00,
    0x0f, 0x00, 0x00, 0x00, 0x1d, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x05, 0x2b, 0xb8, 0x00,
    0x07, 0xac, 0x00, 0x00, 0x00, 0x01, 0x00, 0x10, 0x00, 0x00, 0x00, 0x06, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x08, 0x00, 0x01, 0x00, 0x12, 0x00, 0x00, 0x00, 0x02, 0x00, 0x13
};
static const size_t s_dispatcher_class_bytes_len = sizeof(s_dispatcher_class_bytes);

static const unsigned char s_offset_mapping_class_bytes[] = {
    0xca, 0xfe, 0xba, 0xbe, 0x00, 0x00, 0x00, 0x34, 0x00, 0x1b, 0x0a, 0x00, 0x02, 0x00, 0x03, 0x07,
    0x00, 0x04, 0x0c, 0x00, 0x05, 0x00, 0x06, 0x01, 0x00, 0x10, 0x6a, 0x61, 0x76, 0x61, 0x2f, 0x6c,
    0x61, 0x6e, 0x67, 0x2f, 0x4f, 0x62, 0x6a, 0x65, 0x63, 0x74, 0x01, 0x00, 0x06, 0x3c, 0x69, 0x6e,
    0x69, 0x74, 0x3e, 0x01, 0x00, 0x03, 0x28, 0x29, 0x56, 0x09, 0x00, 0x08, 0x00, 0x09, 0x07, 0x00,
    0x0a, 0x0c, 0x00, 0x0b, 0x00, 0x0c, 0x01, 0x00, 0x31, 0x63, 0x6f, 0x6d, 0x2f, 0x6d, 0x61, 0x6e,
    0x64, 0x61, 0x67, 0x72, 0x61, 0x6d, 0x2f, 0x70, 0x6c, 0x75, 0x67, 0x69, 0x6e, 0x2f, 0x6d, 0x61,
    0x74, 0x68, 0x74, 0x61, 0x62, 0x2f, 0x4d, 0x61, 0x74, 0x68, 0x54, 0x61, 0x62, 0x4f, 0x66, 0x66,
    0x73, 0x65, 0x74, 0x4d, 0x61, 0x70, 0x70, 0x69, 0x6e, 0x67, 0x01, 0x00, 0x07, 0x6f, 0x72, 0x69,
    0x67, 0x4c, 0x65, 0x6e, 0x01, 0x00, 0x01, 0x49, 0x09, 0x00, 0x08, 0x00, 0x0e, 0x0c, 0x00, 0x0f,
    0x00, 0x0c, 0x01, 0x00, 0x08, 0x74, 0x72, 0x61, 0x6e, 0x73, 0x4c, 0x65, 0x6e, 0x07, 0x00, 0x11,
    0x01, 0x00, 0x2c, 0x61, 0x6e, 0x64, 0x72, 0x6f, 0x69, 0x64, 0x78, 0x2f, 0x63, 0x6f, 0x6d, 0x70,
    0x6f, 0x73, 0x65, 0x2f, 0x75, 0x69, 0x2f, 0x74, 0x65, 0x78, 0x74, 0x2f, 0x69, 0x6e, 0x70, 0x75,
    0x74, 0x2f, 0x4f, 0x66, 0x66, 0x73, 0x65, 0x74, 0x4d, 0x61, 0x70, 0x70, 0x69, 0x6e, 0x67, 0x01,
    0x00, 0x05, 0x28, 0x49, 0x49, 0x29, 0x56, 0x01, 0x00, 0x04, 0x43, 0x6f, 0x64, 0x65, 0x01, 0x00,
    0x0f, 0x4c, 0x69, 0x6e, 0x65, 0x4e, 0x75, 0x6d, 0x62, 0x65, 0x72, 0x54, 0x61, 0x62, 0x6c, 0x65,
    0x01, 0x00, 0x15, 0x6f, 0x72, 0x69, 0x67, 0x69, 0x6e, 0x61, 0x6c, 0x54, 0x6f, 0x54, 0x72, 0x61,
    0x6e, 0x73, 0x66, 0x6f, 0x72, 0x6d, 0x65, 0x64, 0x01, 0x00, 0x04, 0x28, 0x49, 0x29, 0x49, 0x01,
    0x00, 0x0d, 0x53, 0x74, 0x61, 0x63, 0x6b, 0x4d, 0x61, 0x70, 0x54, 0x61, 0x62, 0x6c, 0x65, 0x01,
    0x00, 0x15, 0x74, 0x72, 0x61, 0x6e, 0x73, 0x66, 0x6f, 0x72, 0x6d, 0x65, 0x64, 0x54, 0x6f, 0x4f,
    0x72, 0x69, 0x67, 0x69, 0x6e, 0x61, 0x6c, 0x01, 0x00, 0x0a, 0x53, 0x6f, 0x75, 0x72, 0x63, 0x65,
    0x46, 0x69, 0x6c, 0x65, 0x01, 0x00, 0x19, 0x4d, 0x61, 0x74, 0x68, 0x54, 0x61, 0x62, 0x4f, 0x66,
    0x66, 0x73, 0x65, 0x74, 0x4d, 0x61, 0x70, 0x70, 0x69, 0x6e, 0x67, 0x2e, 0x6a, 0x61, 0x76, 0x61,
    0x00, 0x31, 0x00, 0x08, 0x00, 0x02, 0x00, 0x01, 0x00, 0x10, 0x00, 0x02, 0x00, 0x12, 0x00, 0x0b,
    0x00, 0x0c, 0x00, 0x00, 0x00, 0x12, 0x00, 0x0f, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x03, 0x00, 0x01,
    0x00, 0x05, 0x00, 0x12, 0x00, 0x01, 0x00, 0x13, 0x00, 0x00, 0x00, 0x33, 0x00, 0x02, 0x00, 0x03,
    0x00, 0x00, 0x00, 0x0f, 0x2a, 0xb7, 0x00, 0x01, 0x2a, 0x1b, 0xb5, 0x00, 0x07, 0x2a, 0x1c, 0xb5,
    0x00, 0x0d, 0xb1, 0x00, 0x00, 0x00, 0x01, 0x00, 0x14, 0x00, 0x00, 0x00, 0x12, 0x00, 0x04, 0x00,
    0x00, 0x00, 0x09, 0x00, 0x04, 0x00, 0x0a, 0x00, 0x09, 0x00, 0x0b, 0x00, 0x0e, 0x00, 0x0c, 0x00,
    0x01, 0x00, 0x15, 0x00, 0x16, 0x00, 0x01, 0x00, 0x13, 0x00, 0x00, 0x00, 0x51, 0x00, 0x02, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x24, 0x1b, 0x9c, 0x00, 0x05, 0x03, 0xac, 0x1b, 0x2a, 0xb4, 0x00, 0x07,
    0xa4, 0x00, 0x08, 0x2a, 0xb4, 0x00, 0x0d, 0xac, 0x1b, 0x2a, 0xb4, 0x00, 0x0d, 0xa3, 0x00, 0x07,
    0x1b, 0xa7, 0x00, 0x07, 0x2a, 0xb4, 0x00, 0x0d, 0xac, 0x00, 0x00, 0x00, 0x02, 0x00, 0x14, 0x00,
    0x00, 0x00, 0x0e, 0x00, 0x03, 0x00, 0x00, 0x00, 0x10, 0x00, 0x06, 0x00, 0x11, 0x00, 0x13, 0x00,
    0x12, 0x00, 0x17, 0x00, 0x00, 0x00, 0x07, 0x00, 0x04, 0x06, 0x0c, 0x0b, 0x43, 0x01, 0x00, 0x01,
    0x00, 0x18, 0x00, 0x16, 0x00, 0x01, 0x00, 0x13, 0x00, 0x00, 0x00, 0x51, 0x00, 0x02, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x24, 0x1b, 0x9c, 0x00, 0x05, 0x03, 0xac, 0x1b, 0x2a, 0xb4, 0x00, 0x0d, 0xa4,
    0x00, 0x08, 0x2a, 0xb4, 0x00, 0x07, 0xac, 0x1b, 0x2a, 0xb4, 0x00, 0x07, 0xa3, 0x00, 0x07, 0x1b,
    0xa7, 0x00, 0x07, 0x2a, 0xb4, 0x00, 0x07, 0xac, 0x00, 0x00, 0x00, 0x02, 0x00, 0x14, 0x00, 0x00,
    0x00, 0x0e, 0x00, 0x03, 0x00, 0x00, 0x00, 0x17, 0x00, 0x06, 0x00, 0x18, 0x00, 0x13, 0x00, 0x19,
    0x00, 0x17, 0x00, 0x00, 0x00, 0x07, 0x00, 0x04, 0x06, 0x0c, 0x0b, 0x43, 0x01, 0x00, 0x01, 0x00,
    0x19, 0x00, 0x00, 0x00, 0x02, 0x00, 0x1a
};
static const size_t s_offset_mapping_class_bytes_len = sizeof(s_offset_mapping_class_bytes);

static jboolean JNICALL native_on_key_event(JNIEnv* env, jclass, jobject e) {
    if (!env || !e) return JNI_FALSE;
    if (!ensure_jni_cache(env)) return JNI_FALSE;

    jint id = env->CallIntMethod(e, g_cache.mid_key_get_id);
    jint keyCode = env->CallIntMethod(e, g_cache.mid_key_get_code);

    if (keyCode == 18) {
        if (id == 401) g_alt_pressed.store(true);
        else if (id == 402) g_alt_pressed.store(false);
        std::lock_guard<std::mutex> lock(g_state_mutex);
        if (g_active_draft && g_active_on_change) {
            jobject tfv = env->CallObjectMethod(g_active_draft, g_cache.mid_draft_get_value);
            if (tfv) {
                env->CallObjectMethod(g_active_on_change, g_cache.mid_func1_invoke, tfv);
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
        }
        return JNI_FALSE;
    }

    if (id != 401) return JNI_FALSE;
    if (keyCode != 9) return JNI_FALSE;

    jboolean isAlt = env->CallBooleanMethod(e, g_cache.mid_key_is_alt);
#if defined(_WIN32)
    if (!isAlt && ((GetKeyState(VK_MENU) & 0x8000) != 0)) {
        isAlt = JNI_TRUE;
    }
#endif
    if (!isAlt && g_alt_pressed.load()) {
        isAlt = JNI_TRUE;
    }

    jboolean isShift = env->CallBooleanMethod(e, g_cache.mid_key_is_shift);
#if defined(_WIN32)
    if (!isShift && ((GetKeyState(VK_SHIFT) & 0x8000) != 0)) {
        isShift = JNI_TRUE;
    }
#endif

    bool is_replace = (isAlt == JNI_TRUE) || (isShift == JNI_TRUE);

    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (!g_active_draft || !g_active_on_change) return JNI_FALSE;

    jobject tfv = env->CallObjectMethod(g_active_draft, g_cache.mid_draft_get_value);
    if (!tfv) return JNI_FALSE;

    jstring jText = (jstring)env->CallObjectMethod(tfv, g_cache.mid_tfv_get_text);
    if (!jText) return JNI_FALSE;

    const char* rawChars = env->GetStringUTFChars(jText, nullptr);
    if (!rawChars) {
        env->DeleteLocalRef(jText);
        return JNI_FALSE;
    }
    std::string fullText(rawChars);
    env->ReleaseStringUTFChars(jText, rawChars);
    env->DeleteLocalRef(jText);

    jlong selection = env->CallLongMethod(tfv, g_cache.mid_tfv_get_selection);
    int cursor_u16 = (int)(selection & 0xFFFFFFFFL);
    int cursor_u8 = utf16_to_utf8_offset(fullText, cursor_u16);
    if (cursor_u8 <= 0) return JNI_FALSE;

    int cand_u8_start = 0;
    std::string candidate;
    if (!find_math_candidate(fullText, cursor_u8, cand_u8_start, candidate)) {
        return JNI_FALSE;
    }

    int cand_u8_end = cand_u8_start + (int)candidate.length();
    int end_u8 = cursor_u8;
    if (cand_u8_end > end_u8) end_u8 = cand_u8_end;

    if (is_already_solved(candidate)) {
        if (is_replace) {
            size_t eq_pos = candidate.rfind('=');
            if (eq_pos != std::string::npos) {
                std::string after_eq = candidate.substr(eq_pos + 1);
                size_t first = after_eq.find_first_not_of(" \t");
                if (first != std::string::npos) {
                    std::string solution = after_eq.substr(first);
                    std::string new_full_text = fullText.substr(0, cand_u8_start) + solution + fullText.substr(end_u8);
                    int new_cursor_u8 = cand_u8_start + (int)solution.length();
                    int new_cursor_u16 = utf8_to_utf16_offset(new_full_text, new_cursor_u8);
                    jlong new_selection = ((jlong)new_cursor_u16 << 32) | ((jlong)new_cursor_u16 & 0xFFFFFFFFL);
                    jstring jNewText = env->NewStringUTF(new_full_text.c_str());
                    jobject newTfv = env->CallStaticObjectMethod(g_cache.cls_tfv, g_cache.mid_tfv_copy, tfv, jNewText, new_selection, nullptr, 4, nullptr);
                    env->DeleteLocalRef(jNewText);
                    if (newTfv) {
                        env->CallObjectMethod(g_active_on_change, g_cache.mid_func1_invoke, newTfv);
                        env->DeleteLocalRef(newTfv);
                        if (env->ExceptionCheck()) env->ExceptionClear();
                    }
                }
            }
        }
        env->CallVoidMethod(e, g_cache.mid_key_consume);
        return JNI_TRUE;
    }

    std::string solution;
    bool is_equation = false;
    if (!evaluate_candidate_cached(candidate, solution, is_equation)) {
        return JNI_FALSE;
    }

    std::string replacement;
    if (is_equation) {
        if (is_replace) {
            replacement = solution;
        } else {
            replacement = candidate + " (" + solution + ")";
        }
    } else {
        if (is_replace) {
            replacement = solution;
        } else {
            if (!candidate.empty() && candidate.back() == '=') {
                replacement = candidate + solution;
            } else {
                replacement = candidate + "=" + solution;
            }
        }
    }

    std::string new_full_text = fullText.substr(0, cand_u8_start) + replacement + fullText.substr(end_u8);
    int new_cursor_u8 = cand_u8_start + (int)replacement.length();
    int new_cursor_u16 = utf8_to_utf16_offset(new_full_text, new_cursor_u8);
    jlong new_selection = ((jlong)new_cursor_u16 << 32) | ((jlong)new_cursor_u16 & 0xFFFFFFFFL);

    jstring jNewText = env->NewStringUTF(new_full_text.c_str());
    jobject newTfv = env->CallStaticObjectMethod(g_cache.cls_tfv, g_cache.mid_tfv_copy, tfv, jNewText, new_selection, nullptr, 4, nullptr);
    env->DeleteLocalRef(jNewText);
    if (!newTfv) return JNI_FALSE;

    env->CallObjectMethod(g_active_on_change, g_cache.mid_func1_invoke, newTfv);
    env->DeleteLocalRef(newTfv);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return JNI_FALSE;
    }

    env->CallVoidMethod(e, g_cache.mid_key_consume);

    if (g_api) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Evaluated: %s -> %s", candidate.c_str(), replacement.c_str());
        g_api->log_info(TAG, buf);
    }

    return JNI_TRUE;
}

static void before_visual_transform(HookParam* param) {
    if (!param) return;
    param->return_early = 0;
    param->has_result = 0;
    param->result = nullptr;

    if (!param->args || param->args_count < 3) return;
    void* raw_handle = param->args[2];
    if (!raw_handle) return;

    JNIEnv* env = get_jni_env();
    if (!env) return;
    if (!ensure_jni_cache(env)) return;

    jobject raw = env->CallStaticObjectMethod(g_cache.cls_scope, g_cache.mid_get_object, (jlong)raw_handle);
    if (!raw) return;

    jstring jText = (jstring)env->CallObjectMethod(raw, g_cache.mid_annotated_get_text);
    if (!jText) return;

    jint orig_u16_len = env->GetStringLength(jText);
    if (orig_u16_len <= 0) {
        env->DeleteLocalRef(jText);
        return;
    }

    const char* chars = env->GetStringUTFChars(jText, nullptr);
    if (!chars) {
        env->DeleteLocalRef(jText);
        return;
    }
    std::string text(chars);
    env->ReleaseStringUTFChars(jText, chars);
    env->DeleteLocalRef(jText);

    if (text.empty()) return;

    int cand_u8_start = 0;
    std::string candidate;
    if (!find_math_candidate(text, (int)text.length(), cand_u8_start, candidate)) {
        return;
    }

    std::string solution;
    bool is_equation = false;
    if (!evaluate_candidate_cached(candidate, solution, is_equation)) return;

    int cand_u16_start = utf8_to_utf16_offset(text, cand_u8_start);
    int cand_u16_len = utf8_to_utf16_offset(candidate, (int)candidate.length());
    int cand_u16_end = cand_u16_start + cand_u16_len;
    if (cand_u16_end > orig_u16_len) cand_u16_end = orig_u16_len;
    if (cand_u16_start > cand_u16_end) cand_u16_start = cand_u16_end;

    bool is_alt = g_alt_pressed.load();
#if defined(_WIN32)
    if (!is_alt && ((GetKeyState(VK_MENU) & 0x8000) != 0)) {
        is_alt = true;
    }
#endif

    jobject builder = nullptr;

    if (is_alt) {
        builder = env->NewObject(g_cache.cls_builder, g_cache.mid_builder_empty);
        if (!builder) return;

        if (cand_u16_start > 0) {
            env->CallVoidMethod(builder, g_cache.mid_append_range, raw, 0, cand_u16_start);
        }
        env->CallIntMethod(builder, g_cache.mid_push_style, g_cache.span_style);
        jstring jSol = env->NewStringUTF(solution.c_str());
        env->CallVoidMethod(builder, g_cache.mid_append_str, jSol);
        env->DeleteLocalRef(jSol);
        env->CallVoidMethod(builder, g_cache.mid_pop);

        if (cand_u16_end < orig_u16_len) {
            env->CallVoidMethod(builder, g_cache.mid_append_range, raw, cand_u16_end, orig_u16_len);
        }
    } else {
        std::string ghost;
        if (is_equation) {
            ghost = " (" + solution + ")";
        } else {
            if (!candidate.empty() && candidate.back() == '=') {
                ghost = solution;
            } else {
                ghost = "=" + solution;
            }
        }

        builder = env->NewObject(g_cache.cls_builder, g_cache.mid_builder_copy, raw);
        if (!builder) return;

        env->CallIntMethod(builder, g_cache.mid_push_style, g_cache.span_style);
        jstring jGhost = env->NewStringUTF(ghost.c_str());
        env->CallVoidMethod(builder, g_cache.mid_append_str, jGhost);
        env->DeleteLocalRef(jGhost);
        env->CallVoidMethod(builder, g_cache.mid_pop);
    }

    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        if (builder) env->DeleteLocalRef(builder);
        return;
    }

    jobject newAnnotated = env->CallObjectMethod(builder, g_cache.mid_to_annotated);
    env->DeleteLocalRef(builder);
    if (!newAnnotated) return;

    jstring jTransText = (jstring)env->CallObjectMethod(newAnnotated, g_cache.mid_annotated_get_text);
    if (!jTransText) {
        env->DeleteLocalRef(newAnnotated);
        return;
    }
    jint trans_u16_len = env->GetStringLength(jTransText);
    env->DeleteLocalRef(jTransText);

    jobject mappingObj = env->NewObject(g_cache.cls_mapping, g_cache.mid_mapping_init, orig_u16_len, trans_u16_len);
    if (!mappingObj) {
        env->DeleteLocalRef(newAnnotated);
        return;
    }

    jobject transformed = env->NewObject(g_cache.cls_transformed, g_cache.mid_trans_init, newAnnotated, mappingObj);
    env->DeleteLocalRef(newAnnotated);
    env->DeleteLocalRef(mappingObj);
    if (!transformed) return;

    jlong handle = env->CallStaticLongMethod(g_cache.cls_scope, g_cache.mid_to_handle, transformed);
    env->DeleteLocalRef(transformed);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return;
    }

    param->result = (void*)(uintptr_t)handle;
    param->return_early = 1;
    param->has_result = 1;
}

static void before_message_input_bar(HookParam* param) {
    if (!param || !param->args || param->args_count < 2) return;
    void* draft_handle = param->args[0];
    void* on_change_handle = param->args[1];
    if (!draft_handle || !on_change_handle) return;

    JNIEnv* env = get_jni_env();
    if (!env) return;

    jobject draft = get_object_from_handle(env, draft_handle);
    jobject on_change = get_object_from_handle(env, on_change_handle);
    if (!draft || !on_change) return;

    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (g_active_draft) {
        env->DeleteGlobalRef(g_active_draft);
        g_active_draft = nullptr;
    }
    if (g_active_on_change) {
        env->DeleteGlobalRef(g_active_on_change);
        g_active_on_change = nullptr;
    }
    g_active_draft = env->NewGlobalRef(draft);
    g_active_on_change = env->NewGlobalRef(on_change);
}

extern "C" PLUGIN_EXPORT bool mandagram_plugin_init(const PluginApi* api) {
    if (!api) return false;
    g_api = api;

    g_api->log_info(TAG, "Initializing MathTab v1.2.1...");

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

        jclass clsDispatcher = find_app_class(env, "com.mandagram.plugin.mathtab.MathTabDispatcher");
        if (!clsDispatcher) {
            clsDispatcher = env->DefineClass(
                "com/mandagram/plugin/mathtab/MathTabDispatcher",
                g_class_loader,
                (const jbyte*)s_dispatcher_class_bytes,
                (jsize)s_dispatcher_class_bytes_len
            );
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
            }
        }

        if (clsDispatcher) {
            JNINativeMethod nativeMethods[] = {
                {(char*)"onKeyEvent", (char*)"(Ljava/awt/event/KeyEvent;)Z", (void*)native_on_key_event}
            };
            jint regRes = env->RegisterNatives(clsDispatcher, nativeMethods, 1);
            if (regRes == 0) {
                jmethodID midInit = env->GetMethodID(clsDispatcher, "<init>", "()V");
                if (midInit) {
                    jobject dispatcherInstance = env->NewObject(clsDispatcher, midInit);
                    if (dispatcherInstance) {
                        jclass clsKfm = env->FindClass("java/awt/KeyboardFocusManager");
                        if (clsKfm) {
                            jmethodID midGetKfm = env->GetStaticMethodID(clsKfm, "getCurrentKeyboardFocusManager", "()Ljava/awt/KeyboardFocusManager;");
                            jobject kfm = env->CallStaticObjectMethod(clsKfm, midGetKfm);
                            if (kfm) {
                                jmethodID midAddDisp = env->GetMethodID(clsKfm, "addKeyEventDispatcher", "(Ljava/awt/KeyEventDispatcher;)V");
                                if (midAddDisp) {
                                    env->CallVoidMethod(kfm, midAddDisp, dispatcherInstance);
                                    g_api->log_info(TAG, "AWT KeyEventDispatcher registered successfully.");
                                }
                            }
                        }
                    }
                }
            } else {
                g_api->log_error(TAG, "Failed to register natives on MathTabDispatcher.");
            }
        } else {
            g_api->log_error(TAG, "Failed to define MathTabDispatcher class.");
        }

        jclass clsMapping = find_app_class(env, "com.mandagram.plugin.mathtab.MathTabOffsetMapping");
        if (!clsMapping) {
            clsMapping = env->DefineClass(
                "com/mandagram/plugin/mathtab/MathTabOffsetMapping",
                g_class_loader,
                (const jbyte*)s_offset_mapping_class_bytes,
                (jsize)s_offset_mapping_class_bytes_len
            );
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
            }
        }
        if (clsMapping) {
            g_api->log_info(TAG, "MathTabOffsetMapping defined successfully.");
        } else {
            g_api->log_error(TAG, "Failed to define MathTabOffsetMapping class.");
        }

        ensure_jni_cache(env);
    }

    int32_t hook_res = g_api->register_hook(
        "com.mandagram.ui.chat.MessageComposerKt",
        "MessageInputBar",
        "(Lcom/mandagram/ui/chat/ComposerDraft;Lkotlin/jvm/functions/Function1;Lkotlin/jvm/functions/Function0;Lkotlin/jvm/functions/Function0;ZLkotlin/jvm/functions/Function0;Landroidx/compose/ui/focus/FocusRequester;ZLkotlin/jvm/functions/Function1;Lkotlin/jvm/functions/Function1;Lkotlin/jvm/functions/Function1;Landroidx/compose/runtime/Composer;III)V",
        before_message_input_bar,
        nullptr
    );

    if (hook_res == 0) {
        g_api->log_info(TAG, "Successfully hooked MessageComposerKt.MessageInputBar");
    } else {
        g_api->log_error(TAG, "Failed to hook MessageComposerKt.MessageInputBar");
    }

    int32_t trans_res = g_api->register_hook(
        "com.mandagram.ui.chat.MessageComposerKt$MessageInputBar$3$1$2$1",
        "invoke$lambda$1$lambda$0",
        "(Lcom/mandagram/ui/chat/ComposerDraft;JLandroidx/compose/ui/text/AnnotatedString;)Landroidx/compose/ui/text/input/TransformedText;",
        before_visual_transform,
        nullptr
    );

    if (trans_res == 0) {
        g_api->log_info(TAG, "Successfully hooked VisualTransformation for ghost preview");
    } else {
        g_api->log_error(TAG, "Failed to hook VisualTransformation for ghost preview");
    }

    g_api->log_info(TAG, "MathTab v1.2.1 initialized successfully.");
    return true;
}
