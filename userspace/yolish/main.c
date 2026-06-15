#include "yolish.h"

static char src_buf[32768];
static char path_buf[256];

static int read_file(const char *path) {
    int fd = open(path, 0);
    if (fd < 0) return -1;
    int n = (int)read(fd, src_buf, 32767);
    if (n < 0) n = 0;
    src_buf[n] = 0;
    close(fd);
    return n;
}

int main(void) {
    /* Read script path from /ys_run */
    const char *script = "/hello.y";
    int pfd = open("/ys_run", 0);
    if (pfd >= 0) {
        int n = (int)read(pfd, path_buf, 255);
        if (n > 0) {
            /* strip newline */
            while (n > 0 && (path_buf[n-1]=='\n'||path_buf[n-1]=='\r'))
                n--;
            path_buf[n] = 0;
            script = path_buf;
        }
        close(pfd);
    }

    int len = read_file(script);
    if (len < 0) {
        puts("[YS] cannot open: ");
        puts(script);
        puts("\n");
        return 1;
    }

    Lexer l;
    lex_init(&l, src_buf, len);
    Node *prog = parse_program(&l);
    Env *env = env_new(0);
    eval_program(prog, env);
    return 0;
}
