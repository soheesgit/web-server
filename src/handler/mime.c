/* src/handler/mime.c
 *
 * 확장자 → Content-Type. 명세: mime.h
 */
#include "handler/mime.h"

#include <string.h>

#include "core/str.h"

const char *mime_for_path(const char *path)
{
    static const struct {
        const char *extension;
        const char *type;
    } types[] = {
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
    /* 디렉토리 이름의 점은 건너뛰고 파일 이름의 마지막 점을 찾는다. */
    const char *slash = strrchr(path, '/');
    const char *name = slash == NULL ? path : slash + 1;
    const char *dot = strrchr(name, '.');

    /* .profile의 첫 점은 숨김 파일 표시다. x.tar.gz의 확장자는 gz다. */
    if (dot != NULL && dot != name) {
        str_t extension = str_from_cstr(dot + 1);
        for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
            if (str_eq_ci(extension, types[i].extension)) {
                return types[i].type;
            }
        }
    }
    return "application/octet-stream";
}
