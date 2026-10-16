/*
    versions.h - PS5JustDance
    Supported Just Dance PS4 title IDs (2014–2022) and param.sfo parser.

    Licensed under the GNU Lesser General Public License version 2.1, or later.
*/

#ifndef PS5JD_VERSIONS_H
#define PS5JD_VERSIONS_H

#include <stdbool.h>
#include <stdint.h>

/* True if `title_id` is in the built-in Just Dance CUSA list or matches `extra_title_id`. */
bool versions_is_target_title(const char *title_id, const char *extra_title_id);

/* Reads APP_VER from the game's param.sfo. Returns 0 on success. */
int versions_read_app_version(const char *title_id, char *out, unsigned out_size);

/* Reads TITLE from the game's param.sfo and checks if it is a Just Dance game. */
bool versions_is_just_dance_sfo_title(const char *title_id);

/* Parses a string key (e.g. "APP_VER" or "TITLE") out of a param.sfo image in memory. Exposed for testing. */
int versions_parse_sfo_key(const uint8_t *sfo, unsigned size, const char *key, char *out, unsigned out_size);

/* Kept for test compatibility: parses APP_VER out of a param.sfo image. */
int versions_parse_sfo_app_version(const uint8_t *sfo, unsigned size, char *out, unsigned out_size);

#endif
