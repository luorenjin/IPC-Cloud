/**
 * @file json.h
 * @brief L2 —— 最小 JSON DOM（解析 / 查询 / 构建 / 序列化）
 *
 * 面向配置与能力清单等小文档（≤64KB），线性查找即可。不做流式与 SIMD 优化。
 * 所有 getter 对 NULL 输入安全返回默认值，便于链式 json_get(json_get(o,"a"),"b")。
 */
#ifndef IPC_CORE_JSON_H
#define IPC_CORE_JSON_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JSON_NULL = 0, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT
} json_type_t;

typedef struct json json_t;

/* 解析：len=0 表示以 NUL 结尾。失败返回 NULL 并在 err 写入原因（可为 NULL） */
json_t *json_parse(const char *text, size_t len, char *err, size_t errcap);
void    json_free(json_t *j);
json_t *json_clone(const json_t *j);

/* 类型与取值 */
json_type_t json_type(const json_t *j);
bool        json_is(const json_t *j, json_type_t t);
bool        json_bool(const json_t *j, bool def);
double      json_number(const json_t *j, double def);
int64_t     json_int(const json_t *j, int64_t def);
const char *json_string(const json_t *j, const char *def);

/* 容器 */
size_t        json_size(const json_t *j);                       /**< 数组长度 / 对象成员数 */
const json_t *json_get(const json_t *obj, const char *key);     /**< 对象成员 */
const json_t *json_at(const json_t *arr, size_t idx);           /**< 数组元素 / 对象第 idx 个值 */
const char   *json_key_at(const json_t *obj, size_t idx);
/** 点分路径："a.b.0.c"；数字段在数组上按索引、在对象上按键名 */
const json_t *json_path(const json_t *root, const char *path);

/* 构建（返回值由调用者拥有，直到挂到父节点或 json_free） */
json_t *json_new_null(void);
json_t *json_new_bool(bool v);
json_t *json_new_number(double v);
json_t *json_new_int(int64_t v);
json_t *json_new_string(const char *s);
json_t *json_new_array(void);
json_t *json_new_object(void);
/** 挂入对象（接管 val 所有权；同名键替换）。成功 0 */
int     json_object_set(json_t *obj, const char *key, json_t *val);
/** 删除并返回成员（调用者负责 free），不存在返回 NULL */
json_t *json_object_remove(json_t *obj, const char *key);
int     json_array_push(json_t *arr, json_t *val);

/* 序列化：返回 malloc 字符串 */
char *json_dump(const json_t *j, bool pretty);

#ifdef __cplusplus
}
#endif

#endif /* IPC_CORE_JSON_H */
