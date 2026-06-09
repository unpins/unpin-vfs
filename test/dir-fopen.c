/* dir-fopen -- exercises the UNPIN_VFS_DIRS superset against an embedded blob.
 *
 * Built by test/dir-test.sh: a small tree is packed into the blob, this links
 * vfs.c with -DUNPIN_VFS_DIRS and `ld --wrap`, then checks directory listing,
 * directory-aware stat, fopen, and real-path fall-through. The blob layout:
 *
 *   top.txt
 *   a/b/file1.txt   ("hello from file1\n")
 *   a/b/file2.txt
 *   a/c/file3.txt
 */
#include "vfs.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) printf("  ok   " __VA_ARGS__); \
    else { printf("  FAIL " __VA_ARGS__); failures++; } \
    printf("\n"); \
} while (0)

int main(void) {
    struct stat st;

    printf("== directory-aware stat ==\n");
    CHECK(unpin_vfs_stat("/zip/a/b/file1.txt", &st) == 0 && S_ISREG(st.st_mode)
          && st.st_size == 17, "file1.txt is a 17-byte regular file");
    CHECK(unpin_vfs_stat("/zip/a", &st) == 0 && S_ISDIR(st.st_mode),
          "/zip/a is a directory (implicit)");
    CHECK(unpin_vfs_stat("/zip/a/b", &st) == 0 && S_ISDIR(st.st_mode),
          "/zip/a/b is a directory");
    CHECK(unpin_vfs_stat("/zip/nope", &st) == -1, "missing path -> -1");
    CHECK(unpin_vfs_stat("/zip", &st) == 0 && S_ISDIR(st.st_mode),
          "bare mount root /zip (no trailing slash) is a directory");
    CHECK(unpin_vfs_is_virtual("/zip") && unpin_vfs_is_virtual("/zip/a")
          && !unpin_vfs_is_virtual("/zipfoo"), "is_virtual: bare root yes, sibling no");

    printf("== opendir / readdir / closedir ==\n");
    /* /zip/a should list exactly {b, c}; /zip/a/b should list {file1.txt, file2.txt} */
    DIR *d = unpin_vfs_opendir("/zip/a");
    CHECK(d != NULL, "opendir /zip/a");
    int saw_b = 0, saw_c = 0, n = 0;
    struct dirent *e;
    while (d && (e = unpin_vfs_readdir(d))) {
        n++;
        if (!strcmp(e->d_name, "b")) saw_b = 1;
        if (!strcmp(e->d_name, "c")) saw_c = 1;
    }
    if (d) unpin_vfs_closedir(d);
    CHECK(saw_b && saw_c && n == 2, "/zip/a lists exactly {b, c} (n=%d)", n);

    d = unpin_vfs_opendir("/zip/a/b");
    int saw_f1 = 0, saw_f2 = 0, m = 0;
    while (d && (e = unpin_vfs_readdir(d))) {
        m++;
        if (!strcmp(e->d_name, "file1.txt")) saw_f1 = 1;
        if (!strcmp(e->d_name, "file2.txt")) saw_f2 = 1;
    }
    if (d) unpin_vfs_closedir(d);
    CHECK(saw_f1 && saw_f2 && m == 2, "/zip/a/b lists {file1.txt, file2.txt} (m=%d)", m);

    CHECK(unpin_vfs_opendir("/zip/a/b/file1.txt") == NULL,
          "opendir on a file -> NULL");

    /* opendir with a trailing slash must still list children (vim's
     * unix_expandpath opens "dir/"). */
    d = unpin_vfs_opendir("/zip/a/b/");
    int ts_n = 0;
    while (d && (e = unpin_vfs_readdir(d))) ts_n++;
    if (d) unpin_vfs_closedir(d);
    CHECK(ts_n == 2, "opendir(\"/zip/a/b/\") trailing slash lists 2 (n=%d)", ts_n);

    /* the mount root itself must list its top-level children */
    d = unpin_vfs_opendir("/zip/");
    int saw_top = 0, saw_a = 0, r = 0;
    while (d && (e = unpin_vfs_readdir(d))) {
        r++;
        if (!strcmp(e->d_name, "top.txt")) saw_top = 1;
        if (!strcmp(e->d_name, "a")) saw_a = 1;
    }
    if (d) unpin_vfs_closedir(d);
    CHECK(saw_top && saw_a && r == 2, "/zip/ (mount root) lists {top.txt, a} (r=%d)", r);

    printf("== fopen ==\n");
    FILE *f = unpin_vfs_fopen("/zip/a/b/file1.txt", "r");
    char buf[64] = {0};
    size_t got = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    if (f) fclose(f);
    CHECK(f && got == 17 && !strcmp(buf, "hello from file1\n"),
          "fopen+fread reads file1 contents");
    CHECK(unpin_vfs_fopen("/zip/a/b/file1.txt", "w") == NULL,
          "fopen write-mode on virtual path -> NULL (EROFS)");

    printf("== real-path fall-through ==\n");
    /* a real file the VFS must NOT intercept */
    char tmpl[] = "/tmp/uvfs_realXXXXXX";
    int fd = mkstemp(tmpl);
    if (fd >= 0) { write(fd, "real", 4); close(fd); }
    CHECK(unpin_vfs_stat(tmpl, &st) == 0 && st.st_size == 4,
          "stat on a real /tmp file falls through to libc");
    f = unpin_vfs_fopen(tmpl, "r");
    CHECK(f != NULL, "fopen on a real file falls through");
    if (f) fclose(f);
    unlink(tmpl);

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
