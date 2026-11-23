/* src/handler/mime.c
 *
 * 확장자 → Content-Type. 명세: mime.h
 */
#include "handler/mime.h"

#include <stddef.h>

static unsigned char lower_ascii(unsigned char c)
{
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
}

static int eq_ext(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        if (lower_ascii((unsigned char)*a) != lower_ascii((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

const char *mime_for_path(const char *path)
{
    const char *slash, *dot, *ext;
    if (path == NULL)
        return "application/octet-stream";
    slash = path;
    for (const char *p = path; *p != '\0'; p++)
        if (*p == '/')
            slash = p + 1;
    dot = NULL;
    for (const char *p = slash; *p != '\0'; p++)
        if (*p == '.')
            dot = p;
    if (dot == NULL || dot == slash)
        return "application/octet-stream";
    ext = dot + 1;
#define MIME(name, type)                                                                           \
    if (eq_ext(ext, name))                                                                         \
    return type
    MIME("html", "text/html; charset=utf-8");
    MIME("htm", "text/html; charset=utf-8");
    MIME("css", "text/css; charset=utf-8");
    MIME("js", "text/javascript; charset=utf-8");
    MIME("mjs", "text/javascript; charset=utf-8");
    MIME("txt", "text/plain; charset=utf-8");
    MIME("json", "application/json");
    MIME("svg", "image/svg+xml");
    MIME("png", "image/png");
    MIME("jpg", "image/jpeg");
    MIME("jpeg", "image/jpeg");
    MIME("gif", "image/gif");
    MIME("ico", "image/x-icon");
    MIME("webp", "image/webp");
    MIME("pdf", "application/pdf");
    MIME("woff2", "font/woff2");
    MIME("mp4", "video/mp4");
    MIME("gz", "application/gzip");
#undef MIME
    return "application/octet-stream";
}
