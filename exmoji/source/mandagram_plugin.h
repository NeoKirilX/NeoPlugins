#ifndef MANDAGRAM_PLUGIN_H
#define MANDAGRAM_PLUGIN_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#if defined(_WIN32) || defined(__CYGWIN__)
  #define PLUGIN_EXPORT __declspec(dllexport)
#else
  #define PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PluginMetadata {
    const char* name;
    const char* version;
    const char* author;
    const char* description;
} PluginMetadata;

typedef struct HookParam {
    void*    this_ptr;
    void**   args;
    uint32_t args_count;
    uint32_t padding;
    void*    result;
    uint8_t  return_early;
    uint8_t  has_result;
    uint16_t reserved;
    uint32_t padding2;
} HookParam;

#if defined(__x86_64__) || defined(_M_X64)
#ifdef __cplusplus
static_assert(sizeof(HookParam) == 40, "HookParam must be exactly 40 bytes on x64");
#endif
#endif

typedef void (*HookCallback)(HookParam* param);
typedef bool (*UpdateCallback)(const char* json, void* user_data);
typedef const char* (*MessageSendBeforeCallback)(int64_t chat_id, const char* text, uintptr_t user_data);
typedef void (*MessageSendAfterCallback)(int64_t chat_id, int64_t message_id, uintptr_t user_data);
typedef void (*MessageReceiveCallback)(int64_t chat_id, int64_t message_id, const char* text, void* user_data);
typedef void (*ChatActionCallback)(int64_t chat_id, void* user_data);
typedef void (*ComposableFn)(void* composer, int32_t changed, void* user_data);
typedef void (*ScopedComposableFn)(void* scope, void* composer, int32_t changed, void* user_data);

typedef struct PluginApi {
    int32_t  (*register_hook)(const char* class_name, const char* method_name, const char* descriptor, HookCallback before_cb, HookCallback after_cb);
    void     (*log_info)(const char* tag, const char* msg);
    void     (*log_error)(const char* tag, const char* msg);
    int32_t  (*get_storage_dir)(const char* plugin_name, char* out_buf, int32_t max_len);
    int32_t  (*get_cache_dir)(const char* plugin_name, char* out_buf, int32_t max_len);
    int32_t  (*resolve_asset)(const char* plugin_name, const char* asset_name, char* out_buf, int32_t max_len);
    int64_t  (*get_current_user_id)(void);
    int64_t  (*get_active_chat_id)(void);
    int32_t  (*execute_tdlib_query)(const char* json, char* out_buf, int32_t max_len);

    void*    (*create_composable_lambda)(ComposableFn cb, void* user_data);
    void*    (*create_scoped_lambda)(ScopedComposableFn cb, void* user_data);
    void     (*replace_composable)(HookParam* param, ComposableFn cb, void* user_data);
    void     (*composer_start_group)(void* composer, int32_t key);
    void     (*composer_end_group)(void* composer);
    void*    (*composer_get_remembered)(void* composer);
    void     (*composer_set_remembered)(void* composer, void* val);
    bool     (*composer_is_skipping)(void* composer);

    int32_t  (*register_update_hook)(UpdateCallback cb, void* user_data);
    int32_t  (*register_message_send_hook)(MessageSendBeforeCallback before_cb, MessageSendAfterCallback after_cb, uintptr_t user_data);
    int32_t  (*register_message_receive_hook)(MessageReceiveCallback cb, void* user_data);
    int32_t  (*register_chat_action)(const char* id, const char* label, ChatActionCallback click_cb, void* user_data);

    void*    (*get_object_field)(void* obj, const char* field_name);
    void     (*set_object_field)(void* obj, const char* field_name, void* val);
    int32_t  (*get_int_field)(void* obj, const char* field_name);
    void     (*set_int_field)(void* obj, const char* field_name, int32_t val);
    int64_t  (*get_long_field)(void* obj, const char* field_name);
    void     (*set_long_field)(void* obj, const char* field_name, int64_t val);
    bool     (*get_bool_field)(void* obj, const char* field_name);
    void     (*set_bool_field)(void* obj, const char* field_name, bool val);
    int32_t  (*get_string_utf)(void* jstr, char* out_buf, int32_t max_len);
    void*    (*create_string_utf)(const char* str);
    void*    (*get_attached_field)(void* obj, const char* key);
    void     (*set_attached_field)(void* obj, const char* key, void* val);
    void*    (*get_state_value)(void* state_obj);
    void     (*set_state_value)(void* state_obj, void* val);

    void*    (*modifier_empty)(void);
    void*    (*modifier_size)(void* mod, float width_dp, float height_dp);
    void*    (*modifier_width)(void* mod, float width_dp);
    void*    (*modifier_height)(void* mod, float height_dp);
    void*    (*modifier_fill_max_width)(void* mod);
    void*    (*modifier_fill_max_height)(void* mod);
    void*    (*modifier_padding)(void* mod, float start, float top, float end, float bottom);
    void*    (*modifier_background)(void* mod, uint32_t argb, float corner_radius_dp);
    void*    (*modifier_border)(void* mod, float width_dp, uint32_t argb, float corner_radius_dp);
    void*    (*modifier_clickable)(void* mod, void* on_click_lambda);
    void*    (*modifier_weight)(void* scope, void* mod, float weight);

    void     (*compose_row)(void* composer, void* mod, void* content_lambda);
    void     (*compose_column)(void* composer, void* mod, void* content_lambda);
    void     (*compose_box)(void* composer, void* mod, void* content_lambda);
    void     (*compose_spacer)(void* composer, void* mod);
    void     (*compose_text)(void* composer, const char* text, void* mod, uint32_t argb, float font_size_sp, bool is_bold, int32_t max_lines);
    void     (*compose_image)(void* composer, void* bitmap, void* mod);
    void*    (*get_avatar_bitmap)(const char* path);
} PluginApi;

#if defined(__x86_64__) || defined(_M_X64)
#ifdef __cplusplus
static_assert(sizeof(PluginApi) == 53 * sizeof(void*), "PluginApi must have exactly 53 slots");
#endif
#endif

PLUGIN_EXPORT const PluginMetadata* mandagram_plugin_metadata(void);
PLUGIN_EXPORT bool mandagram_plugin_init(const PluginApi* api);

#ifdef __cplusplus
}
#endif

#endif
