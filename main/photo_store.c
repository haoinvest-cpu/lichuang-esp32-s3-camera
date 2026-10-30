#include "photo_store.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bsp_sd.h"
#include "esp_log.h"

static const char *TAG = "photo";

static int s_next_index = 1;
static int s_count = 0;         // 卡上 IMG_xxxx.jpg 的张数

bool photo_store_name_valid(const char *name)
{
    if (!name) {
        return false;
    }
    int idx = 0;
    char tail[2];
    // %1s 用来卡住 ".jpg" 之后的杂字符：精确匹配时返回值是 1。
    // 再叠加长度 12，确保是 IMG_%04d.jpg，且不可能出现 '/' 或 '..'。
    if (sscanf(name, "IMG_%4d.jpg%1s", &idx, tail) != 1) {
        return false;
    }
    return strlen(name) == PHOTO_NAME_LEN;
}

// 扫 /sdcard/DCIM/IMG_xxxx.jpg，取最大序号 +1，并统计张数。
static void scan_existing(void)
{
    DIR *dir = opendir(PHOTO_DIR);
    if (!dir) {
        ESP_LOGW(TAG, "%s not found, start at IMG_0001", PHOTO_DIR);
        s_next_index = 1;
        s_count = 0;
        return;
    }

    int max_idx = 0;
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        int idx = 0;
        char tail[2];
        if (sscanf(ent->d_name, "IMG_%4d.jpg%1s", &idx, tail) == 1
            && strlen(ent->d_name) == PHOTO_NAME_LEN) {
            count++;
            if (idx > max_idx) {
                max_idx = idx;
            }
        }
    }
    closedir(dir);

    s_next_index = max_idx + 1;
    s_count = count;
    ESP_LOGI(TAG, "found IMG_0001..IMG_%04d, next=%d, count=%d", max_idx, s_next_index, s_count);
}

esp_err_t photo_store_begin(void)
{
    if (!bsp_sd_mounted()) {
        s_next_index = 1;
        s_count = 0;
        return ESP_ERR_INVALID_STATE;
    }
    // 目录已存在时 mkdir 返回 -1/EEXIST，忽略即可
    if (mkdir(PHOTO_DIR, 0777) != 0) {
        ESP_LOGD(TAG, "mkdir %s returned -1 (probably EEXIST)", PHOTO_DIR);
    }
    scan_existing();
    return ESP_OK;
}

esp_err_t photo_store_save_jpeg(const uint8_t *data, size_t len,
                                char *rel_path, size_t rel_len,
                                char *full_path, size_t full_len)
{
    if (!bsp_sd_mounted() || !data || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    int idx = s_next_index;
    snprintf(rel_path, rel_len, PHOTO_DIR_REL "/IMG_%04d.jpg", idx);
    snprintf(full_path, full_len, PHOTO_DIR "/IMG_%04d.jpg", idx);

    FILE *fp = fopen(full_path, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "fopen(%s) failed", full_path);
        return ESP_FAIL;
    }
    size_t written = fwrite(data, 1, len, fp);
    if (written != len) {
        ESP_LOGE(TAG, "fwrite %u/%u bytes", (unsigned)written, (unsigned)len);
        fclose(fp);
        unlink(full_path);
        return ESP_FAIL;
    }
    fflush(fp);
    fsync(fileno(fp));      // 回看/断电前把数据落到卡上
    fclose(fp);

    s_next_index = idx + 1;
    s_count++;
    return ESP_OK;
}

int photo_store_count(void)
{
    return s_count;
}

static int cmp_desc(const void *a, const void *b)
{
    int x = *(const int *)a;
    int y = *(const int *)b;
    return (x < y) - (x > y);
}

int photo_store_list(uint16_t *idx_out, int max_entries)
{
    if (!idx_out || max_entries <= 0) {
        return 0;
    }

    DIR *dir = opendir(PHOTO_DIR);
    if (!dir) {
        return 0;
    }

    // 第一遍只数总数，第二遍才收序号：避免动态数组扩容。
    int total = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        int idx = 0;
        char tail[2];
        if (sscanf(ent->d_name, "IMG_%4d.jpg%1s", &idx, tail) == 1
            && strlen(ent->d_name) == PHOTO_NAME_LEN) {
            total++;
        }
    }
    if (total == 0) {
        closedir(dir);
        return 0;
    }

    int *all = (int *)malloc(sizeof(int) * total);
    if (!all) {
        closedir(dir);
        return 0;
    }

    rewinddir(dir);
    int n = 0;
    while ((ent = readdir(dir)) != NULL && n < total) {
        int idx = 0;
        char tail[2];
        if (sscanf(ent->d_name, "IMG_%4d.jpg%1s", &idx, tail) == 1
            && strlen(ent->d_name) == PHOTO_NAME_LEN) {
            all[n++] = idx;
        }
    }
    closedir(dir);

    qsort(all, n, sizeof(int), cmp_desc);   // 最新（序号最大）在前
    int out = (n < max_entries) ? n : max_entries;
    for (int i = 0; i < out; i++) {
        idx_out[i] = (uint16_t)all[i];
    }
    free(all);
    return n;
}

esp_err_t photo_store_delete(const char *name)
{
    if (!bsp_sd_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
    // 红线：先严格校验，非法绝不拼路径
    if (!photo_store_name_valid(name)) {
        ESP_LOGW(TAG, "delete rejected: invalid name");
        return ESP_ERR_INVALID_ARG;
    }

    char path[48];
    snprintf(path, sizeof(path), PHOTO_DIR "/%s", name);
    if (unlink(path) != 0) {
        ESP_LOGW(TAG, "unlink(%s) failed", path);
        return ESP_ERR_NOT_FOUND;
    }

    if (s_count > 0) {
        s_count--;
    }
    ESP_LOGI(TAG, "deleted %s, count=%d", path, s_count);
    return ESP_OK;
}
