/**
 * @file console_assets.h
 * @brief 内嵌前端资源表
 */
#ifndef CONSOLE_ASSETS_H
#define CONSOLE_ASSETS_H

typedef struct {
    const char          *path;           /**< URL 路径，如 "/index.html" */
    const char          *content_type;
    const unsigned char *data;           /**< gzip 压缩后的内容 */
    unsigned int         len;
    int                  gzipped;        /**< 恒为 1；预留未压缩资源的可能 */
    const char          *etag;           /**< 原始内容 sha256 前 16 位十六进制 */
} console_asset_t;

/** 按精确路径查找；未命中返回 NULL */
const console_asset_t *console_asset_find(const char *path);
/** 返回以 path==NULL 结尾的资源数组 */
const console_asset_t *console_asset_index(void);

#endif /* CONSOLE_ASSETS_H */
