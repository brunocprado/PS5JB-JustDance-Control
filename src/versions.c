/*
    versions.c - PS5JustDance
    Licensed under the GNU Lesser General Public License version 2.1, or later.
*/

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "versions.h"

/*
    Built-in PS4 CUSA Title IDs for Just Dance (run on PS5 via backward compatibility):
      - Just Dance 2014: CUSA00189 (EU), CUSA00195 (US)
      - Just Dance 2015: CUSA00676 (US), CUSA00677 (EU), CUSA01524 (CN)
      - Just Dance 2016: CUSA02450 (US), CUSA02480 (EU)
      - Just Dance 2017: CUSA05586 (EU), CUSA05614 (US)
      - Just Dance 2018: CUSA08367 (EU), CUSA08420 (US), CUSA09301 / CUSA09312 (Demo)
      - Just Dance 2019: CUSA12565 (US), CUSA12595 (EU), CUSA13764 (Asia)
      - Just Dance 2020: CUSA15669 (EU), CUSA15714 (US)
      - Just Dance 2021: CUSA19594 (EU), CUSA19621 (US), CUSA24665 (JP/Asia)
      - Just Dance 2022: CUSA27792 (EU), CUSA27802 (US), CUSA29327 (Asia)
*/
static const char *const target_titles[] = {
    "CUSA00189", "CUSA00195",
    "CUSA00676", "CUSA00677", "CUSA01524",
    "CUSA02450", "CUSA02480",
    "CUSA05586", "CUSA05614",
    "CUSA08367", "CUSA08420", "CUSA09301", "CUSA09312",
    "CUSA12565", "CUSA12595", "CUSA13764",
    "CUSA15669", "CUSA15714",
    "CUSA19594", "CUSA19621", "CUSA24665",
    "CUSA27792", "CUSA27802", "CUSA29327",
};

static bool contains_icase(const char *haystack, const char *needle) {
    if (haystack == NULL || needle == NULL || *needle == '\0')
        return false;
    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p != '\0'; p++) {
        size_t i = 0;
        while (i < nlen && p[i] != '\0' &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nlen)
            return true;
    }
    return false;
}

bool versions_is_target_title(const char *title_id, const char *extra_title_id) {
    if (title_id == NULL || title_id[0] == '\0')
        return false;
    for (unsigned i = 0; i < sizeof(target_titles) / sizeof(target_titles[0]); i++) {
        if (strcasecmp(title_id, target_titles[i]) == 0)
            return true;
    }
    if (extra_title_id != NULL && extra_title_id[0] != '\0' && strcasecmp(title_id, extra_title_id) == 0)
        return true;
    return false;
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int versions_parse_sfo_key(const uint8_t *sfo, unsigned size, const char *key, char *out, unsigned out_size) {
    if (sfo == NULL || size < 20 || key == NULL || out == NULL || out_size == 0)
        return -1;
    if (memcmp(sfo, "\0PSF", 4) != 0)
        return -1;

    uint32_t key_table = rd32(sfo + 8);
    uint32_t data_table = rd32(sfo + 12);
    uint32_t count = rd32(sfo + 16);
    size_t klen = strlen(key);

    for (uint32_t i = 0; i < count; i++) {
        uint32_t entry = 20 + i * 16;
        if (entry + 16 > size)
            return -1;
        uint32_t key_off = key_table + rd16(sfo + entry);
        uint32_t data_len = rd32(sfo + entry + 4);
        uint32_t data_off = data_table + rd32(sfo + entry + 12);
        if (key_off + klen >= size || data_off >= size)
            continue;
        if (memcmp(sfo + key_off, key, klen + 1) != 0)
            continue;
        if (data_off + data_len > size)
            return -1;

        unsigned n = data_len < out_size - 1 ? data_len : out_size - 1;
        memcpy(out, sfo + data_off, n);
        out[n] = '\0';
        while (n > 0 && (out[n - 1] == '\0' || out[n - 1] == ' '))
            out[--n] = '\0';
        return n > 0 ? 0 : -1;
    }
    return -1;
}

int versions_parse_sfo_app_version(const uint8_t *sfo, unsigned size, char *out, unsigned out_size) {
    return versions_parse_sfo_key(sfo, size, "APP_VER", out, out_size);
}

static int read_sfo_key_from_disk(const char *title_id, const char *key, char *out, unsigned out_size) {
    static const char *const templates[] = {
        "/system_data/priv/appmeta/%s/param.sfo",
        "/user/appmeta/%s/param.sfo",
        "/user/app/%s/sce_sys/param.sfo",
        "/mnt/sandbox/%s_000/app0/sce_sys/param.sfo",
    };

    for (unsigned i = 0; i < sizeof(templates) / sizeof(templates[0]); i++) {
        char path[160];
        snprintf(path, sizeof(path), templates[i], title_id);
        FILE *f = fopen(path, "rb");
        if (f == NULL)
            continue;
        uint8_t buf[4096];
        size_t n = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        if (versions_parse_sfo_key(buf, (unsigned)n, key, out, out_size) == 0)
            return 0;
    }
    return -1;
}

int versions_read_app_version(const char *title_id, char *out, unsigned out_size) {
    return read_sfo_key_from_disk(title_id, "APP_VER", out, out_size);
}

bool versions_is_just_dance_sfo_title(const char *title_id) {
    char title[128];
    if (read_sfo_key_from_disk(title_id, "TITLE", title, sizeof(title)) != 0)
        return false;
    return contains_icase(title, "Just Dance") || strstr(title, "舞力全开") != NULL;
}
