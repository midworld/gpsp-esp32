/* HTTP file transfer for the SD card: browse, download, upload, rename
 * and delete.
 *
 *   GET /                    the page (transfer.html)
 *   GET /api/list?path=<dir> the folder as JSON: name, dir, size
 *   GET /files/<path>        download
 *   PUT /files/<path>        upload
 *   MOVE /files/<path>       rename to the Destination header, /files/<new>
 *   DELETE /files/<path>     delete
 *
 * PUT, MOVE and DELETE need the X-gpSP header, so pages on other sites
 * cannot change the card.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gpsp_transfer.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "gpsp_storage.h"

#define TRANSFER_ROOT "/sdcard"
#define TRANSFER_PATH_MAX 300
#define TRANSFER_CHUNK (16 * 1024)
#define TRANSFER_WORK_DIR TRANSFER_ROOT "/.gpsp-upload"
#define TRANSFER_OWNER TRANSFER_WORK_DIR "/owner"
#define TRANSFER_TARGET TRANSFER_WORK_DIR "/target"
#define TRANSFER_PART TRANSFER_WORK_DIR "/part"
#define TRANSFER_BACKUP TRANSFER_WORK_DIR "/backup"
#define TRANSFER_CLIENTS 7
/* Receive timeouts in a row before an upload gives up. */
#define TRANSFER_RECV_RETRIES 5

static const char *TAG = "gpsp_transfer";
static httpd_handle_t s_server;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static struct gpsp_transfer_status s_status;
static atomic_bool s_cancel;
/* Shutdown and close share this lock so a descriptor cannot be reused. */
static SemaphoreHandle_t s_socket_lock;
static int s_sockets[TRANSFER_CLIENTS];
static unsigned s_socket_count;
/* One request at a time: the server runs a single task. The SD card
   reads and writes it by DMA; otherwise its driver needs a bounce buffer
   in internal memory, which Wi-Fi leaves too little of. */
static uint8_t *s_buffer;

extern const char s_page_start[] asm("_binary_transfer_html_start");
extern const char s_page_end[] asm("_binary_transfer_html_end");

static int transfer_hex(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c = (char)tolower((unsigned char)c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* Undoes %XX escapes in place. */
static bool transfer_unescape(char *text)
{
    char *out = text;

    for (const char *in = text; *in; in++)
    {
        if (*in == '%')
        {
            int high = transfer_hex(in[1]);
            int low = high < 0 ? -1 : transfer_hex(in[2]);

            if (low < 0 || !(high | low))
                return false;
            *out++ = (char)(high << 4 | low);
            in += 2;
        }
        else
            *out++ = *in;
    }
    *out = '\0';
    return true;
}

static bool transfer_relative_path(char *path)
{
    const char *part;

    if (path[0] != '/')
        return false;
    for (const char *c = path; *c; c++)
        if ((unsigned char)*c < 0x20 || (unsigned char)*c == 0x7f ||
                strchr("\\:*?\"<>|", *c))
            return false;
    /* A folder may end in '/'; drop it. */
    size_t length = strlen(path);

    while (length > 1 && path[length - 1] == '/')
        path[--length] = '\0';
    for (part = path + 1; length > 1 && part;)
    {
        const char *end = strchr(part, '/');
        size_t part_length = end ? (size_t)(end - part) : strlen(part);

        /* FatFs checks the name's UTF-16 length when resolving it. */
        if (!part_length ||
                part[part_length - 1] == '.' || part[part_length - 1] == ' ')
            return false;
        part = end ? end + 1 : NULL;
    }
    return true;
}

static bool transfer_work_path(const char *path)
{
    size_t length = strlen(TRANSFER_WORK_DIR);

    return !strncasecmp(path, TRANSFER_WORK_DIR, length) &&
            (!path[length] || path[length] == '/');
}

/* Decode and resolve aliases before checking the private work folder. */
static bool transfer_path(const char *encoded, char *path, size_t size)
{
    char decoded[CONFIG_HTTPD_MAX_URI_LEN + 1];
    int written;

    if (snprintf(decoded, sizeof(decoded), "%s", encoded) >=
            (int)sizeof(decoded) || !transfer_unescape(decoded) ||
            !transfer_relative_path(decoded))
        return false;
    written = snprintf(path, size, TRANSFER_ROOT "%s",
            decoded[1] ? decoded : "");
    if (written <= 0 || (size_t)written >= size ||
            !gpsp_storage_card_path(path, decoded, sizeof(decoded)) ||
            transfer_work_path(decoded))
        return false;
    written = snprintf(path, size, "%s", decoded);
    return written > 0 && (size_t)written < size;
}

static const char s_owner[] = "gpSP upload\n";

/* 1: ours, 0: absent or unrelated, -1: card error. */
static int transfer_owned(void)
{
    char owner[sizeof(s_owner)];
    FILE *file = fopen(TRANSFER_OWNER, "rb");
    bool ok;

    if (!file)
        return errno == ENOENT || errno == ENOTDIR ? 0 : -1;
    ok = fread(owner, 1, sizeof(owner), file) == sizeof(s_owner) - 1 &&
            !memcmp(owner, s_owner, sizeof(s_owner) - 1);
    bool failed = ferror(file);

    if (fclose(file) != 0)
        failed = true;
    return failed ? -1 : ok;
}

static bool transfer_remove(const char *path)
{
    return remove(path) == 0 || errno == ENOENT;
}

/* The owner marker survives between uploads. Never adopt a user's files. */
static bool transfer_work_init(void)
{
    struct stat info;
    struct dirent *entry;
    char owner[sizeof(s_owner)];
    size_t length = 0;
    FILE *file;
    DIR *dir;
    bool ok;
    int owned;

    if (stat(TRANSFER_WORK_DIR, &info) != 0)
    {
        if (errno != ENOENT || mkdir(TRANSFER_WORK_DIR, 0700) != 0)
            return false;
    }
    else if (!S_ISDIR(info.st_mode))
        return false;
    owned = transfer_owned();
    if (owned < 0)
        return false;
    if (owned)
        return true;

    file = fopen(TRANSFER_OWNER, "rb");
    if (file)
    {
        length = fread(owner, 1, sizeof(owner), file);
        ok = !ferror(file) && length < sizeof(s_owner) &&
                !memcmp(owner, s_owner, length);
        if (fclose(file) != 0)
            ok = false;
        if (!ok)
            return false;
    }
    else if (errno != ENOENT)
        return false;

    /* A reset while making the marker can leave an empty folder or prefix. */
    dir = opendir(TRANSFER_WORK_DIR);
    if (!dir)
        return false;
    ok = true;
    for (;;)
    {
        errno = 0;
        entry = readdir(dir);
        if (!entry)
        {
            ok = errno == 0;
            break;
        }
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..") &&
                strcmp(entry->d_name, "owner"))
        {
            ok = false;
            break;
        }
    }
    closedir(dir);
    if (!ok)
        return false;
    file = fopen(TRANSFER_OWNER, "wb");
    if (!file)
        return false;
    ok = fwrite(s_owner, 1, sizeof(s_owner) - 1, file) == sizeof(s_owner) - 1;
    if (fclose(file) != 0)
        ok = false;
    return ok;
}

esp_err_t gpsp_transfer_recover(void)
{
    char target[TRANSFER_PATH_MAX + 1];
    char actual[TRANSFER_PATH_MAX];
    struct stat info;
    size_t length = 0;
    size_t root = strlen(TRANSFER_ROOT);
    bool backup;
    bool valid = false;
    FILE *file;
    int owned = transfer_owned();

    if (owned < 0)
        return ESP_FAIL;
    if (!owned)
        return ESP_OK;
    file = fopen(TRANSFER_TARGET, "rb");
    if (file)
    {
        length = fread(target, 1, sizeof(target), file);
        bool failed = ferror(file);

        valid = !failed && length > root + 2 && length < sizeof(target) &&
                target[length - 1] == '\0' && strlen(target) == length - 1 &&
                !strncmp(target, TRANSFER_ROOT "/", root + 1) &&
                transfer_relative_path(target + root) &&
                gpsp_storage_card_path(target, actual, sizeof(actual)) &&
                !transfer_work_path(actual);
        if (fclose(file) != 0 || failed)
            return ESP_FAIL;
    }
    else if (errno != ENOENT)
        return ESP_FAIL;

    backup = stat(TRANSFER_BACKUP, &info) == 0;
    if ((!backup && errno != ENOENT) || (backup && S_ISDIR(info.st_mode)))
        return ESP_FAIL;
    if (backup)
    {
        /* A bad record must never make us discard the only original. */
        if (!valid)
            return ESP_FAIL;
        if (stat(actual, &info) == 0)
        {
            if (S_ISDIR(info.st_mode) || !transfer_remove(TRANSFER_BACKUP))
                return ESP_FAIL;
        }
        else if (errno != ENOENT || rename(TRANSFER_BACKUP, actual) != 0)
            return ESP_FAIL;
    }
    if (!transfer_remove(TRANSFER_PART) || !transfer_remove(TRANSFER_TARGET))
        return ESP_FAIL;
    return ESP_OK;
}

static FILE *transfer_upload_begin(const char *path)
{
    FILE *file;
    bool ok;
    size_t length = strlen(path) + 1;

    if (!transfer_work_init() || gpsp_transfer_recover() != ESP_OK)
        return NULL;
    file = fopen(TRANSFER_TARGET, "wb");
    if (!file)
        return NULL;
    /* Closing commits the target record before data or backups can exist. */
    ok = fwrite(path, 1, length, file) == length;
    if (fclose(file) != 0)
        ok = false;
    if (!ok)
    {
        transfer_remove(TRANSFER_TARGET);
        return NULL;
    }
    file = fopen(TRANSFER_PART, "wb");
    if (!file)
        gpsp_transfer_recover();
    return file;
}

static void transfer_set_file(const char *path, bool upload, uint32_t total)
{
    const char *slash = strrchr(path, '/');
    char name[sizeof(s_status.file)] = {0};

    /* Formatted outside the lock, which keeps interrupts off. */
    snprintf(name, sizeof(name), "%s", slash ? slash + 1 : path);
    portENTER_CRITICAL(&s_lock);
    memcpy(s_status.file, name, sizeof(name));
    s_status.upload = upload;
    s_status.done = 0;
    s_status.total = total;
    portEXIT_CRITICAL(&s_lock);
}

static void transfer_progress(uint32_t done)
{
    portENTER_CRITICAL(&s_lock);
    s_status.done = done;
    portEXIT_CRITICAL(&s_lock);
}

static void transfer_end(bool upload, bool ok, bool game_file)
{
    portENTER_CRITICAL(&s_lock);
    s_status.file[0] = '\0';
    if (ok && upload)
        s_status.uploads++;
    else if (ok)
        s_status.downloads++;
    if (ok && game_file)
        s_status.game_changed = true;
    portEXIT_CRITICAL(&s_lock);
}

/* Files of the running game: the ROM, its cartridge save and its states. */
static bool transfer_game_file(const char *path)
{
    const char *rom = gpsp_storage_rom_path();
    const char *save = gpsp_storage_save_path();
    const char *state = gpsp_storage_state_path();
    char actual[TRANSFER_PATH_MAX];

    return (rom && gpsp_storage_card_path(rom, actual, sizeof(actual)) &&
                    !strcasecmp(path, actual)) ||
            (save && gpsp_storage_card_path(save, actual, sizeof(actual)) &&
                    !strcasecmp(path, actual)) ||
            (state && gpsp_storage_card_path(state, actual, sizeof(actual)) &&
                    !strncasecmp(path, actual, strlen(actual)));
}

static esp_err_t transfer_page(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    /* The embedded text ends in a terminating zero. */
    return httpd_resp_send(request, s_page_start,
            s_page_end - s_page_start - 1);
}

/* Sends text as a JSON string. */
static esp_err_t transfer_json_string(httpd_req_t *request, const char *text)
{
    char out[2 * TRANSFER_PATH_MAX];
    size_t length = 0;

    out[length++] = '"';
    for (const char *c = text; *c && length < sizeof(out) - 8; c++)
    {
        unsigned char byte = (unsigned char)*c;

        if (byte == '"' || byte == '\\')
        {
            out[length++] = '\\';
            out[length++] = (char)byte;
        }
        else if (byte < 0x20)
            length += (size_t)snprintf(out + length, 7, "\\u%04x", byte);
        else
            out[length++] = (char)byte;
    }
    out[length++] = '"';
    return httpd_resp_send_chunk(request, out, (ssize_t)length);
}

static esp_err_t transfer_list(httpd_req_t *request)
{
    char query[CONFIG_HTTPD_MAX_URI_LEN + 1];
    char value[CONFIG_HTTPD_MAX_URI_LEN + 1];
    char dir_path[TRANSFER_PATH_MAX];
    char entry_path[TRANSFER_PATH_MAX + 256];
    struct dirent *entry;
    bool first = true;
    bool ok = true;
    DIR *dir;

    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
            httpd_query_key_value(query, "path", value, sizeof(value)) != ESP_OK ||
            !transfer_path(value, dir_path, sizeof(dir_path)))
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Bad path");
    dir = opendir(dir_path);
    if (!dir)
        return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "No folder");

    httpd_resp_set_type(request, "application/json");
    ok = httpd_resp_send_chunk(request, "[", 1) == ESP_OK;
    while (ok && !atomic_load(&s_cancel) && (entry = readdir(dir)))
    {
        struct stat info;
        char tail[48];
        bool folder = entry->d_type == DT_DIR;

        /* Hidden files, such as macOS "._" metadata, stay hidden. */
        if (entry->d_name[0] == '.')
            continue;
        snprintf(entry_path, sizeof(entry_path), "%s/%s", dir_path,
                entry->d_name);
        if (!folder && stat(entry_path, &info) != 0)
            continue;
        snprintf(tail, sizeof(tail), ",\"dir\":%s,\"size\":%lu}",
                folder ? "true" : "false",
                folder ? 0ul : (unsigned long)info.st_size);
        ok = httpd_resp_send_chunk(request,
                first ? "{\"name\":" : ",{\"name\":", -1) == ESP_OK &&
                transfer_json_string(request, entry->d_name) == ESP_OK &&
                httpd_resp_send_chunk(request, tail, -1) == ESP_OK;
        first = false;
    }
    closedir(dir);
    if (!ok || atomic_load(&s_cancel) ||
            httpd_resp_send_chunk(request, "]", 1) != ESP_OK)
        return ESP_FAIL;
    return httpd_resp_send_chunk(request, NULL, 0);
}

static esp_err_t transfer_download(httpd_req_t *request)
{
    char path[TRANSFER_PATH_MAX];
    struct stat info;
    uint32_t done = 0;
    size_t read;
    FILE *file;
    bool ok = true;

    if (!transfer_path(request->uri + strlen("/files"), path, sizeof(path)) ||
            stat(path, &info) != 0 || S_ISDIR(info.st_mode))
        return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "No file");
    file = fopen(path, "rb");
    if (!file)
        return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "No file");

    transfer_set_file(path, false, (uint32_t)info.st_size);
    httpd_resp_set_type(request, "application/octet-stream");
    while (done < (uint32_t)info.st_size && !atomic_load(&s_cancel))
    {
        read = fread(s_buffer, 1, TRANSFER_CHUNK, file);
        /* A short read is a card error: the file must not look complete. */
        if (!read)
        {
            ESP_LOGW(TAG, "Read failed: %s", path);
            ok = false;
            break;
        }
        if (httpd_resp_send_chunk(request, (const char *)s_buffer,
                (ssize_t)read) != ESP_OK)
        {
            ok = false;
            break;
        }
        done += (uint32_t)read;
        transfer_progress(done);
    }
    fclose(file);
    ok = ok && !atomic_load(&s_cancel) &&
            httpd_resp_send_chunk(request, NULL, 0) == ESP_OK;
    transfer_end(false, ok, false);
    /* Failing drops the connection, so the browser sees the cut. */
    if (!ok)
        return ESP_FAIL;
    return ESP_OK;
}

/* Puts a finished upload in place of the old file. FAT cannot rename
   over a file: the old one stays as a backup until the new one is in. */
static bool transfer_replace(const char *path)
{
    struct stat info;
    bool had_file = stat(path, &info) == 0;

    if ((had_file && S_ISDIR(info.st_mode)) ||
            (!had_file && errno != ENOENT))
        return false;
    if (had_file)
    {
        if (stat(TRANSFER_BACKUP, &info) == 0 || errno != ENOENT)
            return false;
        if (rename(path, TRANSFER_BACKUP) != 0)
            return false;
    }
    return rename(TRANSFER_PART, path) == 0;
}

static esp_err_t transfer_upload(httpd_req_t *request)
{
    char path[TRANSFER_PATH_MAX];
    char marker[4];
    size_t left = request->content_len;
    uint32_t done = 0;
    int retries = 0;
    FILE *file;
    bool ok = true;
    bool game_file;
    struct stat info;

    if (httpd_req_get_hdr_value_str(request, "X-gpSP", marker,
            sizeof(marker)) != ESP_OK)
        return httpd_resp_send_err(request, HTTPD_403_FORBIDDEN,
                "Missing X-gpSP header");
    if (!transfer_path(request->uri + strlen("/files"), path, sizeof(path)) ||
            !strcmp(path, TRANSFER_ROOT))
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Bad path");
    if (stat(path, &info) == 0 && S_ISDIR(info.st_mode))
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                "Target is a folder");
    game_file = transfer_game_file(path);
    file = transfer_upload_begin(path);
    if (!file)
        return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND,
                "Cannot write there");

    transfer_set_file(path, true, (uint32_t)left);
    while (left > 0 && !atomic_load(&s_cancel))
    {
        int received = httpd_req_recv(request, (char *)s_buffer,
                left < TRANSFER_CHUNK ? left : TRANSFER_CHUNK);

        if (received == HTTPD_SOCK_ERR_TIMEOUT &&
                ++retries < TRANSFER_RECV_RETRIES)
            continue;
        if (received <= 0 ||
                fwrite(s_buffer, 1, (size_t)received, file) != (size_t)received)
        {
            ok = false;
            break;
        }
        retries = 0;
        left -= (size_t)received;
        done += (uint32_t)received;
        transfer_progress(done);
    }
    if (fclose(file) != 0)
        ok = false;
    ok = ok && !left && !atomic_load(&s_cancel) &&
            transfer_replace(path);
    if (gpsp_transfer_recover() != ESP_OK)
        ESP_LOGE(TAG, "Upload recovery pending: %s", path);
    transfer_end(true, ok, game_file);
    if (!ok)
    {
        ESP_LOGW(TAG, "Upload failed: %s", path);
        if (!atomic_load(&s_cancel))
            httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                    "Upload failed");
        /* Do not drain an unfinished request body after failure. */
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Uploaded %s (%lu bytes)", path, (unsigned long)done);
    httpd_resp_set_status(request, "201 Created");
    return httpd_resp_sendstr(request, "OK");
}

/* Deletes one file. The running ROM must remain available for restart. */
static esp_err_t transfer_delete(httpd_req_t *request)
{
    char path[TRANSFER_PATH_MAX];
    char marker[4];
    const char *rom = gpsp_storage_rom_path();
    char actual[TRANSFER_PATH_MAX];
    struct stat info;
    bool game_file;

    if (httpd_req_get_hdr_value_str(request, "X-gpSP", marker,
            sizeof(marker)) != ESP_OK)
        return httpd_resp_send_err(request, HTTPD_403_FORBIDDEN,
                "Missing X-gpSP header");
    /* Pending recovery could otherwise bring the deleted file back. */
    if (gpsp_transfer_recover() != ESP_OK)
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                "Upload recovery pending");
    if (!transfer_path(request->uri + strlen("/files"), path, sizeof(path)) ||
            stat(path, &info) != 0 || S_ISDIR(info.st_mode))
        return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "No file");
    if (rom && gpsp_storage_card_path(rom, actual, sizeof(actual)) &&
            !strcasecmp(path, actual))
        return httpd_resp_send_err(request, HTTPD_403_FORBIDDEN,
                "The running game cannot be deleted");
    game_file = transfer_game_file(path);
    if (remove(path) != 0)
    {
        ESP_LOGW(TAG, "Delete failed: %s", path);
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                "Delete failed");
    }
    if (game_file)
    {
        portENTER_CRITICAL(&s_lock);
        s_status.game_changed = true;
        portEXIT_CRITICAL(&s_lock);
    }
    ESP_LOGI(TAG, "Deleted %s", path);
    return httpd_resp_sendstr(request, "OK");
}

/* Renames one file to a free name, except the running game's ROM. */
static esp_err_t transfer_move(httpd_req_t *request)
{
    char path[TRANSFER_PATH_MAX];
    char target[TRANSFER_PATH_MAX];
    char destination[CONFIG_HTTPD_MAX_URI_LEN + 1];
    char marker[4];
    const char *rom = gpsp_storage_rom_path();
    char actual[TRANSFER_PATH_MAX];
    struct stat info;
    bool game_file;

    if (httpd_req_get_hdr_value_str(request, "X-gpSP", marker,
            sizeof(marker)) != ESP_OK)
        return httpd_resp_send_err(request, HTTPD_403_FORBIDDEN,
                "Missing X-gpSP header");
    /* Pending recovery could later restore the old name. */
    if (gpsp_transfer_recover() != ESP_OK)
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                "Upload recovery pending");
    if (!transfer_path(request->uri + strlen("/files"), path, sizeof(path)) ||
            stat(path, &info) != 0 || S_ISDIR(info.st_mode))
        return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "No file");
    if (httpd_req_get_hdr_value_str(request, "Destination", destination,
            sizeof(destination)) != ESP_OK ||
            strncmp(destination, "/files/", strlen("/files/")) ||
            !transfer_path(destination + strlen("/files"), target,
                    sizeof(target)) || !strcmp(target, TRANSFER_ROOT))
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                "Bad destination");
    /* Names resolve without case, so a case-only change looks taken. */
    if (stat(target, &info) == 0 || errno != ENOENT)
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                "Name already in use");
    if (rom && gpsp_storage_card_path(rom, actual, sizeof(actual)) &&
            !strcasecmp(path, actual))
        return httpd_resp_send_err(request, HTTPD_403_FORBIDDEN,
                "The running game cannot be renamed");
    game_file = transfer_game_file(path) || transfer_game_file(target);
    if (rename(path, target) != 0)
    {
        ESP_LOGW(TAG, "Rename failed: %s", path);
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                "Rename failed");
    }
    if (game_file)
    {
        portENTER_CRITICAL(&s_lock);
        s_status.game_changed = true;
        portEXIT_CRITICAL(&s_lock);
    }
    ESP_LOGI(TAG, "Renamed %s to %s", path, target);
    return httpd_resp_sendstr(request, "OK");
}

static esp_err_t transfer_socket_open(httpd_handle_t server, int fd)
{
    (void)server;
    xSemaphoreTake(s_socket_lock, portMAX_DELAY);
    s_sockets[s_socket_count++] = fd;
    if (atomic_load(&s_cancel))
        shutdown(fd, SHUT_RDWR);
    xSemaphoreGive(s_socket_lock);
    return ESP_OK;
}

static void transfer_socket_close(httpd_handle_t server, int fd)
{
    (void)server;
    xSemaphoreTake(s_socket_lock, portMAX_DELAY);
    for (unsigned i = 0; i < s_socket_count; i++)
        if (s_sockets[i] == fd)
        {
            s_sockets[i] = s_sockets[--s_socket_count];
            break;
        }
    close(fd);
    xSemaphoreGive(s_socket_lock);
}

esp_err_t gpsp_transfer_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    static const httpd_uri_t handlers[] = {
        { .uri = "/", .method = HTTP_GET, .handler = transfer_page },
        { .uri = "/api/list", .method = HTTP_GET, .handler = transfer_list },
        { .uri = "/files/*", .method = HTTP_GET, .handler = transfer_download },
        { .uri = "/files/*", .method = HTTP_PUT, .handler = transfer_upload },
        { .uri = "/files/*", .method = HTTP_MOVE, .handler = transfer_move },
        { .uri = "/files/*", .method = HTTP_DELETE,
            .handler = transfer_delete },
    };
    esp_err_t result;

    if (s_server)
        return ESP_OK;
    result = gpsp_transfer_recover();
    if (result != ESP_OK)
        return result;
    if (!s_socket_lock)
        s_socket_lock = xSemaphoreCreateMutex();
    if (!s_socket_lock)
        return ESP_ERR_NO_MEM;
    if (!s_buffer)
        s_buffer = heap_caps_aligned_alloc(64, TRANSFER_CHUNK,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!s_buffer)
        return ESP_ERR_NO_MEM;
    portENTER_CRITICAL(&s_lock);
    memset(&s_status, 0, sizeof(s_status));
    portEXIT_CRITICAL(&s_lock);
    atomic_store(&s_cancel, false);
    s_socket_count = 0;

    config.stack_size = 8192;
    config.max_open_sockets = TRANSFER_CLIENTS;
    config.open_fn = transfer_socket_open;
    config.close_fn = transfer_socket_close;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;
    result = httpd_start(&s_server, &config);
    for (size_t i = 0; result == ESP_OK && i < sizeof(handlers) /
            sizeof(handlers[0]); i++)
        result = httpd_register_uri_handler(s_server, &handlers[i]);
    if (result != ESP_OK)
    {
        ESP_LOGW(TAG, "Server start failed: %s", esp_err_to_name(result));
        gpsp_transfer_stop();
    }
    return result;
}

void gpsp_transfer_stop(void)
{
    if (s_server)
    {
        atomic_store(&s_cancel, true);
        xSemaphoreTake(s_socket_lock, portMAX_DELAY);
        /* Wake blocked reads and writes, including incomplete headers. */
        for (unsigned i = 0; i < s_socket_count; i++)
            shutdown(s_sockets[i], SHUT_RDWR);
        xSemaphoreGive(s_socket_lock);
        httpd_stop(s_server);
        s_server = NULL;
    }
}

void gpsp_transfer_status(struct gpsp_transfer_status *status)
{
    portENTER_CRITICAL(&s_lock);
    *status = s_status;
    portEXIT_CRITICAL(&s_lock);
}
