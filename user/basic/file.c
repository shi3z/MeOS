#include "file.h"
#include <m5os/vfs.h>
#include <m5os/kmalloc.h>
#include <m5os/errno.h>
#include <string.h>

int basic_file_load(basic_session *s, const char *path, basic_file_result *result)
{
    *result = (basic_file_result){ .detail = "cannot load file" };
    vfs_info_t info;
    int rc = vfs_stat(path, &info);
    if (rc < 0) return rc;
    if (info.type != FS_TYPE_FILE) { result->detail = "not a regular file"; return ERR_INVAL; }
    if (info.size >= BASIC_PROGRAM_CAP) { result->detail = "file too large"; return ERR_NOSPC; }
    basic_program *staged = kmalloc_psram(sizeof *staged);
    if (!staged) staged = kmalloc(sizeof *staged);
    if (!staged) { result->detail = "cannot stage program"; return ERR_NOMEM; }
    int fd = vfs_open(path, VFS_O_READ);
    if (fd < 0) { kfree(staged); return fd; }
    size_t len = 0;
    rc = ERR_OK;
    while (len < sizeof s->program) {
        int n = vfs_read(fd, s->program+len, sizeof s->program-len);
        if (n < 0) { rc = n; break; }
        if (!n) break;
        len += (size_t)n;
    }
    int close_rc = vfs_close(fd);
    result->bytes = len;
    if (rc == ERR_OK && close_rc < 0) rc = close_rc;
    if (rc == ERR_OK && len == sizeof s->program) { result->detail = "file too large"; rc = ERR_NOSPC; }
    if (rc == ERR_OK) {
        const char *error = basic_program_parse(staged, s->program, len, &result->line);
        if (error) { result->detail = error; rc = ERR_CORRUPT; }
        else {
            memcpy(&s->source, staged, sizeof s->source);
            ubasic_init("", s->output, s->opaque, 1);
            result->detail = "loaded";
        }
    }
    kfree(staged);
    return rc;
}
int basic_file_save(basic_session *s, const char *path, basic_file_result *result)
{
    *result = (basic_file_result){ .detail = "cannot save file" };
    vfs_info_t info;
    int rc = vfs_stat(path, &info);
    if (rc == ERR_OK && info.type != FS_TYPE_FILE) { result->detail = "not a regular file"; return ERR_INVAL; }
    if (rc != ERR_OK && rc != ERR_NOENT) return rc;
    size_t len = basic_session_export(s);
    int fd = vfs_open(path, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (fd < 0) return fd;
    rc = ERR_OK;
    while (result->bytes < len) {
        int n = vfs_write(fd, s->program+result->bytes, len-result->bytes);
        if (n < 0) { rc = n; break; }
        if (!n) { rc = ERR_NOSPC; break; }
        result->bytes += (size_t)n;
    }
    int close_rc = vfs_close(fd);
    if (rc == ERR_OK && close_rc < 0) rc = close_rc;
    if (rc == ERR_OK) result->detail = "saved";
    else result->detail = "save failed (file may be partial)";
    return rc;
}
