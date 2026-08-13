/* binding-libc -- exercises the BINDING, not the core.
 *
 * Every other test calls the unpin_vfs_* API directly, so it proves the lookup
 * and inflate paths and nothing about how a consumer's own open/stat/... reach
 * them. This one calls the bare libc names: whichever binding the build selected
 * (--wrap, symbol rename, or dlsym) is the only reason these land in the VFS.
 *
 * Built by test/binding-test.sh in self-EOF mode against the tree it packs:
 *
 *   top.txt
 *   a/b/file1.txt   ("hello from file1\n")
 *   a/b/file2.txt
 *   a/c/file3.txt
 */
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) printf("  ok   " __VA_ARGS__); \
    else { printf("  FAIL " __VA_ARGS__); failures++; } \
    printf("\n"); \
} while (0)

int main(void) {
    struct stat st;
    char buf[64];

    printf("== virtual paths reach the VFS through bare libc calls ==\n");
    int fd = open("/zip/a/b/file1.txt", O_RDONLY);
    ssize_t got = fd >= 0 ? read(fd, buf, sizeof buf - 1) : -1;
    if (fd >= 0) close(fd);
    if (got > 0) buf[got] = '\0';
    CHECK(got == 17 && !strcmp(buf, "hello from file1\n"),
          "open+read serves file1 (got=%zd)", got);

    CHECK(stat("/zip/a/b/file1.txt", &st) == 0 && S_ISREG(st.st_mode)
          && st.st_size == 17, "stat: 17-byte regular file");
    CHECK(lstat("/zip/a", &st) == 0 && S_ISDIR(st.st_mode),
          "lstat: /zip/a is a directory");
    CHECK(access("/zip/top.txt", R_OK) == 0, "access: top.txt readable");
    CHECK(open("/zip/nope", O_RDONLY) == -1, "missing virtual path -> -1");

    FILE *f = fopen("/zip/a/b/file1.txt", "r");
    size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    if (f) fclose(f);
    CHECK(f && n == 17, "fopen+fread serves file1 (n=%zu)", n);

    DIR *d = opendir("/zip/a/b");
    int m = 0, saw1 = 0, saw2 = 0;
    struct dirent *e;
    while (d && (e = readdir(d))) {
        m++;
        if (!strcmp(e->d_name, "file1.txt")) saw1 = 1;
        if (!strcmp(e->d_name, "file2.txt")) saw2 = 1;
    }
    if (d) closedir(d);
    CHECK(saw1 && saw2 && m == 2, "opendir/readdir lists {file1,file2} (m=%d)", m);

    printf("== real paths still reach libc ==\n");
    /* The fall-through half is what a wrong binding breaks silently: under
     * dlsym a missed RTLD_NEXT lookup recurses into our own definition. */
    char tmpl[] = "/tmp/uvfs_bindXXXXXX";
    fd = mkstemp(tmpl);
    if (fd >= 0) { (void)!write(fd, "real", 4); close(fd); }
    CHECK(fd >= 0 && stat(tmpl, &st) == 0 && st.st_size == 4,
          "stat on a real file falls through");
    CHECK(lstat(tmpl, &st) == 0 && st.st_size == 4, "lstat falls through");
    CHECK(access(tmpl, R_OK) == 0, "access falls through");
    fd = open(tmpl, O_RDONLY);
    got = fd >= 0 ? read(fd, buf, sizeof buf - 1) : -1;
    if (fd >= 0) close(fd);
    CHECK(got == 4 && !memcmp(buf, "real", 4), "open+read falls through");
    f = fopen(tmpl, "r");
    n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    if (f) fclose(f);
    CHECK(f && n == 4, "fopen falls through");

    d = opendir("/tmp");
    m = 0;
    while (d && readdir(d)) m++;
    if (d) closedir(d);
    CHECK(m > 0, "opendir(/tmp) falls through (%d entries)", m);
    unlink(tmpl);

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
