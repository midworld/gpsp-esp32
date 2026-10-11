/* gpSP FATFS storage backend.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gpsp_storage.h"
#include "gpsp_transfer.h"

#include <ctype.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_vfs_fat.h"
#include "sdkconfig.h"
#include "wear_levelling.h"

#if CONFIG_GPSP_SD_CARD
#include "diskio_sdmmc.h"
#include "driver/sdmmc_host.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"
#if CONFIG_GPSP_DCACHE_AUTOLOAD
#include "esp_cache.h"
#include "esp_memory_utils.h"
#include "soc/cache_reg.h"
#include "soc/soc.h"
#endif
#endif

#define GPSP_STORAGE_MOUNT_PATH "/storage"
#define GPSP_STORAGE_ROM_PATH GPSP_STORAGE_MOUNT_PATH "/game.gba"
#define GPSP_SAVE_MOUNT_PATH "/saves"
#define GPSP_SD_MOUNT_PATH "/sdcard"
#define GPSP_SD_CONFIG_PATH GPSP_SD_MOUNT_PATH "/gpsp.cfg"
#define GPSP_SD_CONFIG_TEMP GPSP_SD_CONFIG_PATH ".tmp"
#define GPSP_SD_CONFIG_BACKUP GPSP_SD_CONFIG_PATH ".bak"
#define GPSP_SD_ROM_DIR GPSP_SD_MOUNT_PATH "/roms"
#define GPSP_SD_SAVE_DIR GPSP_SD_MOUNT_PATH "/saves"
#define GPSP_SD_STATE_DIR GPSP_SD_MOUNT_PATH "/states"
/* State files add up to 19 characters (".state.auto.png.tmp") to the ROM
   name; a longer name is cut and ends in a hash of the whole name. */
#define GPSP_STATE_STEM_MAX 230
#define GPSP_STATE_STEM_HASH 9
/* The game list picks a card ROM to start next. */
#define GPSP_STORAGE_GAME_LIST (CONFIG_GPSP_SD_CARD && CONFIG_GPSP_MENU)
#define GPSP_RESTART_MAGIC 0x52534d47u

static const char *TAG = "gpsp_storage";
static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;
static const char *s_rom_path = GPSP_STORAGE_ROM_PATH;
#if CONFIG_GPSP_SD_CARD
static sdmmc_card_t *s_sd_card;
static sd_pwr_ctrl_handle_t s_sd_power;
static char s_sd_rom_path[sizeof(GPSP_SD_ROM_DIR) + 256];
static char s_sd_save_path[sizeof(GPSP_SD_SAVE_DIR) + 256];
static char s_sd_state_path[sizeof(GPSP_SD_STATE_DIR) + 256];
#endif
#if GPSP_STORAGE_GAME_LIST
/* The ROM to start next, kept across a software restart; power-on clears
   it. */
struct gpsp_restart_request
{
    uint32_t magic;
    bool skip_auto_load;
    char path[sizeof(GPSP_SD_ROM_DIR) + 256];
};

static RTC_NOINIT_ATTR struct gpsp_restart_request s_restart;
static bool s_skip_auto_load;
#endif
#if CONFIG_GPSP_CARTRIDGE_AUTOSAVE && \
        !CONFIG_GPSP_BENCHMARK_STOP_AFTER_REPORT && \
        !CONFIG_GPSP_BENCHMARK_FRESH_SAVE
static wl_handle_t s_save_wl_handle = WL_INVALID_HANDLE;
#endif

#if CONFIG_GPSP_SD_CARD
#if CONFIG_GPSP_DCACHE_AUTOLOAD
/* A fill started before DMA can finish after the driver's invalidation.
   Drain those fills before invalidating the receive buffer again. */
static esp_err_t gpsp_autoload_settle(void *buff, size_t size)
{
    uint32_t ctrl = REG_READ(CACHE_L1_DCACHE_AUTOLOAD_CTRL_REG);
    esp_err_t result = ESP_OK;

    if (!(ctrl & CACHE_L1_DCACHE_AUTOLOAD_ENA))
        return ESP_OK;
    REG_WRITE(CACHE_L1_DCACHE_AUTOLOAD_CTRL_REG,
            ctrl & ~(CACHE_L1_DCACHE_AUTOLOAD_ENA | CACHE_L1_DCACHE_AUTOLOAD_DONE));
    int64_t deadline = esp_timer_get_time() + 1000;
    while (!(REG_READ(CACHE_L1_DCACHE_AUTOLOAD_CTRL_REG) &
            CACHE_L1_DCACHE_AUTOLOAD_DONE))
    {
        if (esp_timer_get_time() >= deadline)
        {
            result = ESP_ERR_TIMEOUT;
            break;
        }
    }
    if (result == ESP_OK)
        result = esp_cache_msync(buff, size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    REG_WRITE(CACHE_L1_DCACHE_AUTOLOAD_CTRL_REG,
            ctrl & ~CACHE_L1_DCACHE_AUTOLOAD_DONE);
    return result;
}

/* The host hook also covers card initialization and filesystem mounting.
   cmd->data is the actual DMA buffer, including the driver's bounce buffer. */
static esp_err_t gpsp_sd_do_transaction(int slot, sdmmc_command_t *cmd)
{
    esp_err_t result = sdmmc_host_do_transaction(slot, cmd);
    size_t align;

    if (result != ESP_OK || cmd->error != ESP_OK ||
            !(cmd->flags & SCF_CMD_READ) || !cmd->data || !cmd->buflen ||
            !esp_ptr_external_ram(cmd->data))
        return result;

    align = esp_cache_get_line_size_by_addr(cmd->data);
    if (!align || (uintptr_t)cmd->data % align || cmd->buflen % align)
        return ESP_ERR_INVALID_ARG;
    result = gpsp_autoload_settle(cmd->data, cmd->buflen);
    if (result != ESP_OK)
        ESP_LOGE(TAG, "SD receive cache sync failed: %s",
                esp_err_to_name(result));
    return result;
}
#endif

/* Slot 0 has fixed pins on the P4. ESP-Hosted may already own the SDMMC
   controller for slot 1; the host init and slot deinit both allow that. */
static bool gpsp_sd_mount(void)
{
    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    esp_err_t result;

#if CONFIG_FATFS_VOLUME_COUNT < 3
    /* The card would take the volume that the save partition needs. */
    ESP_LOGW(TAG, "SD card skipped: needs CONFIG_FATFS_VOLUME_COUNT >= 3");
    return false;
#endif

#if CONFIG_GPSP_SD_LDO_CHAN >= 0
    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = CONFIG_GPSP_SD_LDO_CHAN,
    };

    result = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &s_sd_power);
    if (result != ESP_OK)
    {
        ESP_LOGW(TAG, "SD power setup failed: %s", esp_err_to_name(result));
        return false;
    }
    host.pwr_ctrl_handle = s_sd_power;
#endif

    host.slot = SDMMC_HOST_SLOT_0;
    /* 40 MHz high speed; card init falls back when a card lacks it. */
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    slot_config.width = 4;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
#if CONFIG_GPSP_DCACHE_AUTOLOAD
    host.do_transaction = gpsp_sd_do_transaction;
#endif

    result = esp_vfs_fat_sdmmc_mount(GPSP_SD_MOUNT_PATH, &host, &slot_config,
            &mount_config, &s_sd_card);
    if (result != ESP_OK)
    {
        ESP_LOGI(TAG, "No SD card: %s", esp_err_to_name(result));
        s_sd_card = NULL;
#if CONFIG_GPSP_SD_LDO_CHAN >= 0
        sd_pwr_ctrl_del_on_chip_ldo(s_sd_power);
        s_sd_power = NULL;
#endif
        return false;
    }

    ESP_LOGI(TAG, "SD card mounted: %s", s_sd_card->cid.name);
    return true;
}

static void gpsp_sd_unmount(void)
{
    if (!s_sd_card)
        return;

    /* With ESP-Hosted on slot 1 the controller stays; IDF logs its refused
       delete as an error. */
    esp_log_level_t level = esp_log_level_get("SD_HOST");

    esp_log_level_set("SD_HOST", ESP_LOG_NONE);
    esp_vfs_fat_sdcard_unmount(GPSP_SD_MOUNT_PATH, s_sd_card);
    esp_log_level_set("SD_HOST", level);
    s_sd_card = NULL;
#if CONFIG_GPSP_SD_LDO_CHAN >= 0
    sd_pwr_ctrl_del_on_chip_ldo(s_sd_power);
    s_sd_power = NULL;
#endif
}

/* Trims spaces and line ends in place. */
static char *gpsp_trim(char *text)
{
    size_t length;

    while (isspace((unsigned char)*text))
        text++;
    length = strlen(text);
    while (length && isspace((unsigned char)text[length - 1]))
        text[--length] = '\0';
    return text;
}

/* Reads one line from the console UART, or returns false on timeout. */
static bool gpsp_console_read_line(char *line, size_t size, uint32_t wait_ms)
{
#if CONFIG_ESP_CONSOLE_UART
    const uart_port_t port = CONFIG_ESP_CONSOLE_UART_NUM;
    int64_t deadline = esp_timer_get_time() + (int64_t)wait_ms * 1000;
    size_t length = 0;
    bool done = false;

    if (uart_driver_install(port, 256, 0, 0, NULL, 0) != ESP_OK)
        return false;
    uart_flush_input(port);

    ESP_LOGI(TAG, "ROM name? (%lu ms)", (unsigned long)wait_ms);
    while (!done)
    {
        int64_t left = deadline - esp_timer_get_time();
        uint8_t byte;

        if (left <= 0 ||
            uart_read_bytes(port, &byte, 1, pdMS_TO_TICKS(left / 1000 + 1)) <= 0)
            break;
        if (byte == '\n' || byte == '\r')
            done = length > 0;
        /* Control bytes mean binary input, not a name: drop the line. */
        else if (byte < 0x20)
            length = 0;
        else if (length + 1 < size)
            line[length++] = (char)byte;
    }
    line[length] = '\0';

    uart_driver_delete(port);
    return done;
#else
    (void)line;
    (void)size;
    (void)wait_ms;
    return false;
#endif
}

bool gpsp_storage_config(const char *key, char *value, size_t size)
{
    FILE *file = fopen(GPSP_SD_CONFIG_PATH, "r");
    size_t length = strlen(key);
    char line[256];
    bool found = false;

    if (!file)
        return false;

    while (!found && fgets(line, sizeof(line), file))
    {
        size_t read = strlen(line);

        /* The buffer may end right before the line break ("\r\n" or "\n");
           a line that goes on is skipped whole, since its pieces could read
           as other keys or cut values. */
        if (read && line[read - 1] != '\n')
        {
            int c = fgetc(file);

            if (c == '\r')
                c = fgetc(file);
            if (c != '\n' && c != EOF)
            {
                while ((c = fgetc(file)) != EOF && c != '\n')
                    ;
                continue;
            }
        }

        char *text = gpsp_trim(line);

        if (!strncmp(text, key, length) && text[length] == '=')
        {
            /* A value cut to fit the buffer would read as another value. */
            int written = snprintf(value, size, "%s",
                    gpsp_trim(text + length + 1));
            found = written > 0 && (size_t)written < size;
        }
    }

    fclose(file);
    return found;
}

/* Copies the config with the given keys set; keys it lacks go last. */
static bool gpsp_config_copy(FILE *in, FILE *out,
        const struct gpsp_storage_setting *settings, size_t count)
{
    uint32_t done = 0;
    char line[256];
    int last = '\n';
    /* Added lines end like the file's lines. */
    const char *ending = "\n";

    while (in && fgets(line, sizeof(line), in))
    {
        size_t read = strlen(line);
        const char *text = line;
        size_t i;

        /* A line longer than the buffer is no setting: copy it whole. */
        if (read && line[read - 1] != '\n' && !feof(in))
        {
            int c;

            fputs(line, out);
            while ((c = fgetc(in)) != EOF)
            {
                fputc(c, out);
                last = c;
                if (c == '\n')
                    break;
            }
            continue;
        }

        if (read >= 2 && line[read - 2] == '\r' && line[read - 1] == '\n')
            ending = "\r\n";
        while (isspace((unsigned char)*text))
            text++;
        for (i = 0; i < count; i++)
        {
            size_t length = strlen(settings[i].key);

            if (!strncmp(text, settings[i].key, length) && text[length] == '=')
                break;
        }
        if (i == count)
        {
            fputs(line, out);
            last = read ? line[read - 1] : last;
            continue;
        }
        /* Keep the line's own ending. */
        fprintf(out, "%s=%s%s", settings[i].key, settings[i].value,
                read >= 2 && line[read - 2] == '\r' ? "\r\n" :
                read && line[read - 1] == '\n' ? "\n" : "");
        last = read && line[read - 1] == '\n' ? '\n' : 0;
        done |= 1u << i;
    }

    for (size_t i = 0; i < count; i++)
        if (!(done & 1u << i))
        {
            if (last != '\n')
                fputs(ending, out);
            fprintf(out, "%s=%s%s", settings[i].key, settings[i].value, ending);
            last = '\n';
        }
    return !ferror(out) && (!in || !ferror(in));
}

bool gpsp_storage_config_set(const struct gpsp_storage_setting *settings,
        size_t count)
{
    FILE *in;
    FILE *out;
    bool valid;
    bool had_file;

    if (!s_sd_card || count > 32)
        return false;

    /* Write aside first so a failed write keeps the old file. */
    in = fopen(GPSP_SD_CONFIG_PATH, "r");
    had_file = in != NULL;
    out = fopen(GPSP_SD_CONFIG_TEMP, "w");
    valid = out && gpsp_config_copy(in, out, settings, count);
    if (in)
        fclose(in);
    if (out && fclose(out) != 0)
        valid = false;

    /* FAT cannot rename over a file: the old one stays as a backup until
       the new one is in place. */
    if (valid && had_file)
    {
        remove(GPSP_SD_CONFIG_BACKUP);
        valid = rename(GPSP_SD_CONFIG_PATH, GPSP_SD_CONFIG_BACKUP) == 0;
    }
    if (valid && rename(GPSP_SD_CONFIG_TEMP, GPSP_SD_CONFIG_PATH) != 0)
    {
        valid = false;
        if (had_file)
            rename(GPSP_SD_CONFIG_BACKUP, GPSP_SD_CONFIG_PATH);
    }
    if (valid)
        remove(GPSP_SD_CONFIG_BACKUP);
    else
    {
        remove(GPSP_SD_CONFIG_TEMP);
        ESP_LOGE(TAG, "Writing %s failed", GPSP_SD_CONFIG_PATH);
    }
    return valid;
}

static bool gpsp_is_rom_name(const char *name)
{
    size_t length = strlen(name);

    /* Hidden files, such as macOS "._" metadata next to each ROM. */
    return name[0] != '.' && length > 4 &&
            !strcasecmp(name + length - 4, ".gba");
}

static size_t gpsp_find_roms_in(const char *dir_path,
        gpsp_storage_rom_found_fn found, void *user)
{
    char path[sizeof(GPSP_SD_ROM_DIR) + 256];
    DIR *dir = opendir(dir_path);
    struct dirent *entry;
    size_t count = 0;

    if (!dir)
        return 0;
    while ((entry = readdir(dir)))
    {
        int written;

        if (entry->d_type == DT_DIR || !gpsp_is_rom_name(entry->d_name))
            continue;
        written = snprintf(path, sizeof(path), "%s/%s", dir_path,
                entry->d_name);
        if (written <= 0 || (size_t)written >= sizeof(path))
            continue;
        count++;
        if (found)
            found(path, user);
    }
    closedir(dir);
    return count;
}

size_t gpsp_storage_find_roms(gpsp_storage_rom_found_fn found, void *user)
{
    if (!s_sd_card)
        return 0;
    return gpsp_find_roms_in(GPSP_SD_ROM_DIR, found, user) +
            gpsp_find_roms_in(GPSP_SD_MOUNT_PATH, found, user);
}

bool gpsp_storage_state_path_for(const char *rom_path, char *path,
        size_t size)
{
    const char *slash = strrchr(rom_path, '/');
    const char *base = slash ? slash + 1 : rom_path;
    const char *dot = strrchr(base, '.');
    int stem = dot && dot != base ? (int)(dot - base) : (int)strlen(base);
    int written;

    if (stem <= GPSP_STATE_STEM_MAX)
        written = snprintf(path, size, GPSP_SD_STATE_DIR "/%.*s.state", stem,
                base);
    else
    {
        uint32_t hash = esp_rom_crc32_le(0, (const uint8_t *)base,
                (uint32_t)stem);
        int cut = GPSP_STATE_STEM_MAX - GPSP_STATE_STEM_HASH;

        /* Never cut inside a UTF-8 character. */
        while (cut > 0 && ((unsigned char)base[cut] & 0xc0) == 0x80)
            cut--;
        written = snprintf(path, size, GPSP_SD_STATE_DIR "/%.*s~%08lx.state",
                cut, base, (unsigned long)hash);
    }
    return written > 0 && (size_t)written < size;
}

static void gpsp_log_rom(const char *path, void *user)
{
    (void)user;
    ESP_LOGI(TAG, "SD ROM: %s", strrchr(path, '/') + 1);
}

/* Uses a card ROM, with saves and states named after it. */
static esp_err_t gpsp_sd_use_rom(const char *path)
{
    struct stat rom_stat;
    int written;

    written = snprintf(s_sd_rom_path, sizeof(s_sd_rom_path), "%s", path);
    if (written <= 0 || (size_t)written >= sizeof(s_sd_rom_path) ||
            stat(s_sd_rom_path, &rom_stat) != 0 || rom_stat.st_size <= 0)
    {
        ESP_LOGE(TAG, "ROM not found: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    const char *base = strrchr(s_sd_rom_path, '/') + 1;
    const char *dot = strrchr(base, '.');
    int stem = dot && dot != base ? (int)(dot - base) : (int)strlen(base);

    mkdir(GPSP_SD_SAVE_DIR, 0777);
    mkdir(GPSP_SD_STATE_DIR, 0777);
    snprintf(s_sd_save_path, sizeof(s_sd_save_path),
            GPSP_SD_SAVE_DIR "/%.*s.sav", stem, base);
    gpsp_storage_state_path_for(s_sd_rom_path, s_sd_state_path,
            sizeof(s_sd_state_path));
    s_rom_path = s_sd_rom_path;
    return ESP_OK;
}

/* Picks a card ROM: a restart request first, then a console line, then
   gpsp.cfg. ESP_ERR_NOT_FOUND means a named ROM is missing; ESP_FAIL
   means no name was given. */
static esp_err_t gpsp_sd_select_rom(void)
{
    char line[256];
    char path[sizeof(s_sd_rom_path)];
    char *name = NULL;
    struct stat rom_stat;

    gpsp_storage_find_roms(gpsp_log_rom, NULL);
#if GPSP_STORAGE_GAME_LIST
    if (s_restart.magic == GPSP_RESTART_MAGIC)
    {
        s_restart.magic = 0;
        s_restart.path[sizeof(s_restart.path) - 1] = '\0';
        s_skip_auto_load = s_restart.skip_auto_load;
        ESP_LOGI(TAG, "ROM from the game list: %s", s_restart.path);
        /* A ROM gone since then brings the game list back. */
        return gpsp_sd_use_rom(s_restart.path) == ESP_OK ? ESP_OK : ESP_FAIL;
    }
#endif
    if (gpsp_console_read_line(line, sizeof(line), CONFIG_GPSP_SD_ROM_WAIT_MS))
        name = gpsp_trim(line);

    /* A blank console line counts as no answer. */
    if (name && name[0])
        ESP_LOGI(TAG, "ROM from console: %s", name);
    else if (gpsp_storage_config("rom", line, sizeof(line)))
    {
        name = line;
        ESP_LOGI(TAG, "ROM from %s: %s", GPSP_SD_CONFIG_PATH, name);
    }

    if (!name || !name[0])
        return ESP_FAIL;

    /* Look in roms/ first, then the card root. */
    snprintf(path, sizeof(path), GPSP_SD_ROM_DIR "/%s", name);
    if (stat(path, &rom_stat) != 0 || rom_stat.st_size <= 0)
        snprintf(path, sizeof(path), GPSP_SD_MOUNT_PATH "/%s", name);
    return gpsp_sd_use_rom(path);
}
#else
bool gpsp_storage_config(const char *key, char *value, size_t size)
{
    (void)key;
    (void)value;
    (void)size;
    return false;
}

bool gpsp_storage_config_set(const struct gpsp_storage_setting *settings,
        size_t count)
{
    (void)settings;
    (void)count;
    return false;
}

size_t gpsp_storage_find_roms(gpsp_storage_rom_found_fn found, void *user)
{
    (void)found;
    (void)user;
    return 0;
}

bool gpsp_storage_state_path_for(const char *rom_path, char *path,
        size_t size)
{
    (void)rom_path;
    (void)path;
    (void)size;
    return false;
}
#endif

esp_err_t gpsp_storage_mount(void)
{
    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
        .use_one_fat = false,
    };
    struct stat rom_stat;
    bool sd_rom = false;
    esp_err_t result;

    s_rom_path = GPSP_STORAGE_ROM_PATH;
#if CONFIG_GPSP_SD_CARD
    if (gpsp_sd_mount())
    {
        struct stat config_stat;

        result = gpsp_transfer_recover();
        if (result != ESP_OK)
            ESP_LOGW(TAG, "Upload recovery failed: %s", esp_err_to_name(result));
        /* Finish a config replacement cut short by a reset. */
        if (stat(GPSP_SD_CONFIG_PATH, &config_stat) != 0 &&
                stat(GPSP_SD_CONFIG_BACKUP, &config_stat) == 0)
            rename(GPSP_SD_CONFIG_BACKUP, GPSP_SD_CONFIG_PATH);
        remove(GPSP_SD_CONFIG_TEMP);
        result = gpsp_sd_select_rom();
        /* A wrong name stops here instead of running another ROM. */
        if (result == ESP_ERR_NOT_FOUND)
        {
            gpsp_sd_unmount();
            return result;
        }
        sd_rom = result == ESP_OK;
#if GPSP_STORAGE_GAME_LIST
        /* With no name the menu picks a ROM, even from an empty card:
           File Transfer can bring the first one. */
        if (!sd_rom)
        {
            s_rom_path = NULL;
            sd_rom = true;
        }
#endif
        if (!sd_rom)
            gpsp_sd_unmount();
    }
#endif

    /* A card ROM does not need the storage partition. */
    result = esp_vfs_fat_spiflash_mount_rw_wl(GPSP_STORAGE_MOUNT_PATH,
            "storage", &mount_config, &s_wl_handle);

    if (result != ESP_OK && !sd_rom)
    {
        gpsp_storage_unmount();
        return result;
    }

    if (s_rom_path &&
            (stat(s_rom_path, &rom_stat) != 0 || rom_stat.st_size <= 0))
    {
        ESP_LOGE(TAG, "ROM not found: %s", s_rom_path);
        gpsp_storage_unmount();
        return ESP_ERR_NOT_FOUND;
    }

    if (s_rom_path)
        ESP_LOGI(TAG, "ROM ready: %s (%lu bytes)", s_rom_path,
                (unsigned long)rom_stat.st_size);

#if CONFIG_GPSP_CARTRIDGE_AUTOSAVE && \
        !CONFIG_GPSP_BENCHMARK_STOP_AFTER_REPORT && \
        !CONFIG_GPSP_BENCHMARK_FRESH_SAVE
    esp_vfs_fat_mount_config_t save_config = mount_config;
    save_config.format_if_mount_failed = true;
    result = esp_vfs_fat_spiflash_mount_rw_wl(GPSP_SAVE_MOUNT_PATH,
            "saves", &save_config, &s_save_wl_handle);
    /* A card ROM saves to the card; the partition only offers an import. */
    if (result != ESP_OK && sd_rom)
        ESP_LOGW(TAG, "Save partition mount failed: %s",
                esp_err_to_name(result));
    else if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "Save partition mount failed: %s",
                esp_err_to_name(result));
        gpsp_storage_unmount();
        return result;
    }
#endif

    return ESP_OK;
}

const char *gpsp_storage_rom_path(void)
{
    return s_rom_path;
}

bool gpsp_storage_card_path(const char *path, char *out, size_t size)
{
#if CONFIG_GPSP_SD_CARD
    char fat_path[300];
    size_t root = strlen(GPSP_SD_MOUNT_PATH);
    size_t length = root;
    BYTE drive;
    char *part;

    if (!s_sd_card || strncmp(path, GPSP_SD_MOUNT_PATH, root) ||
            (path[root] && path[root] != '/') || size <= root)
        return false;
    drive = ff_diskio_get_pdrv_card(s_sd_card);
    if (drive == 0xff || snprintf(fat_path, sizeof(fat_path), "%u:%s",
            (unsigned)drive, path + root) >= (int)sizeof(fat_path))
        return false;
    memcpy(out, GPSP_SD_MOUNT_PATH, root + 1);
    part = strchr(fat_path, '/');
    if (!part || !part[1])
        return true;
    part++;
    while (*part)
    {
        FILINFO info;
        FILINFO entry;
        char *end = strchr(part, '/');
        const char *name;
        int written;
        FRESULT result;

        if (end)
            *end = '\0';
        result = f_stat(fat_path, &info);
        if (result == FR_OK)
        {
            FF_DIR dir;
#if FF_USE_LFN
            const char *short_name = info.altname[0] ? info.altname : info.fname;
#else
            const char *short_name = info.fname;
#endif
            bool found = false;

            /* f_stat can keep the caller's case or 8.3 alias. */
            part[-1] = '\0';
            result = f_opendir(&dir, fat_path);
            part[-1] = '/';
            if (result != FR_OK)
                return false;
            while (f_readdir(&dir, &entry) == FR_OK && entry.fname[0])
            {
#if FF_USE_LFN
                const char *alias = entry.altname[0] ? entry.altname : entry.fname;
#else
                const char *alias = entry.fname;
#endif

                if (!strcasecmp(short_name, alias))
                {
                    found = true;
                    break;
                }
            }
            f_closedir(&dir);
            if (!found)
                return false;
            name = entry.fname;
        }
        else if (result == FR_NO_FILE && !end)
            name = part;
        else
            return false;
        written = snprintf(out + length, size - length, "/%s", name);
        if (written <= 0 || (size_t)written >= size - length)
            return false;
        length += (size_t)written;
        if (!end)
            return true;
        *end = '/';
        part = end + 1;
    }
#else
    (void)path;
    (void)out;
    (void)size;
#endif
    return false;
}

#if GPSP_STORAGE_GAME_LIST
bool gpsp_storage_card(void)
{
    return s_sd_card != NULL;
}

esp_err_t gpsp_storage_use_requested_rom(void)
{
    if (s_restart.magic != GPSP_RESTART_MAGIC)
        return ESP_ERR_NOT_FOUND;
    s_restart.magic = 0;
    s_restart.path[sizeof(s_restart.path) - 1] = '\0';
    return gpsp_storage_set_rom(s_restart.path, s_restart.skip_auto_load);
}

esp_err_t gpsp_storage_set_rom(const char *path, bool skip_auto_load)
{
    esp_err_t result = gpsp_sd_use_rom(path);

    if (result == ESP_OK)
    {
        s_skip_auto_load = skip_auto_load;
        ESP_LOGI(TAG, "ROM ready: %s", s_rom_path);
    }
    return result;
}

void gpsp_storage_request_rom(const char *path, bool skip_auto_load)
{
    snprintf(s_restart.path, sizeof(s_restart.path), "%s", path);
    s_restart.skip_auto_load = skip_auto_load;
    s_restart.magic = GPSP_RESTART_MAGIC;
}

bool gpsp_storage_rom_requested(void)
{
    return s_restart.magic == GPSP_RESTART_MAGIC;
}

bool gpsp_storage_skip_auto_load(void)
{
    return s_skip_auto_load;
}
#endif

const char *gpsp_storage_save_path(void)
{
#if CONFIG_GPSP_SD_CARD
    if (s_rom_path && s_rom_path == s_sd_rom_path)
        return s_sd_save_path;
#endif
    return NULL;
}

const char *gpsp_storage_state_path(void)
{
#if CONFIG_GPSP_SD_CARD
    if (s_rom_path && s_rom_path == s_sd_rom_path)
        return s_sd_state_path;
#endif
    return GPSP_STORAGE_MOUNT_PATH "/game.state";
}

void gpsp_storage_unmount(void)
{
#if CONFIG_GPSP_CARTRIDGE_AUTOSAVE && \
        !CONFIG_GPSP_BENCHMARK_STOP_AFTER_REPORT && \
        !CONFIG_GPSP_BENCHMARK_FRESH_SAVE
    if (s_save_wl_handle != WL_INVALID_HANDLE)
    {
        esp_vfs_fat_spiflash_unmount_rw_wl(GPSP_SAVE_MOUNT_PATH,
                s_save_wl_handle);
        s_save_wl_handle = WL_INVALID_HANDLE;
    }
#endif

#if CONFIG_GPSP_SD_CARD
    gpsp_sd_unmount();
#endif

    if (s_wl_handle == WL_INVALID_HANDLE)
        return;

    esp_vfs_fat_spiflash_unmount_rw_wl(GPSP_STORAGE_MOUNT_PATH, s_wl_handle);
    s_wl_handle = WL_INVALID_HANDLE;
}
