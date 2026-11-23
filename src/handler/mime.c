/* src/handler/mime.c
 *
 * 확장자 → Content-Type. 명세: mime.h
 */
#include "handler/mime.h"

#include <stddef.h>
#include <string.h>
#include <strings.h>

static const struct {
    const char *ext;
    const char *type;
} TABLE[] = {
    {"html", "text/html; charset=utf-8"},
    {"htm", "text/html; charset=utf-8"},
    {"css", "text/css; charset=utf-8"},
    {"js", "text/javascript; charset=utf-8"},
    {"mjs", "text/javascript; charset=utf-8"},
    {"txt", "text/plain; charset=utf-8"},
    {"json", "application/json"},
    {"svg", "image/svg+xml"},
    {"png", "image/png"},
    {"jpg", "image/jpeg"},
    {"jpeg", "image/jpeg"},
    {"gif", "image/gif"},
    {"ico", "image/x-icon"},
    {"webp", "image/webp"},
    {"pdf", "application/pdf"},
    {"woff2", "font/woff2"},
    {"mp4", "video/mp4"},
    {"gz", "application/gzip"},
};

const char *mime_for_path(const char *path)
{
    const char *name = strrchr(path, '/');
    const char *dot;

    name = name != NULL ? name + 1 : path;
    dot = strrchr(name, '.');
    /* '.'이 없거나 이름 맨 앞의 '.'뿐이면 확장자 없음 */
    if (dot != NULL && dot != name) {
        for (size_t i = 0; i < sizeof(TABLE) / sizeof(TABLE[0]); i++) {
            if (strcasecmp(dot + 1, TABLE[i].ext) == 0) {
                return TABLE[i].type;
            }
        }
    }
    return "application/octet-stream";
}
