#include "yolish.h"

static char src_buf[65536];
static char path_buf[256];

static int read_file(const char *path) {
    int fd = open(path, 0);
    if (fd < 0) return -1;
    int n = (int)read(fd, src_buf, sizeof(src_buf) - 1);
    if (n < 0) n = 0;
    src_buf[n] = 0;
    close(fd);
    return n;
}

int main(int argc, char **argv) {
    const char *script = 0;
    const char *inline_code = 0;

    /* Parse CLI arguments if supplied by shell */
    if (argc > 1) {
        if (strcmp_u(argv[1], "-e") == 0 && argc > 2) {
            inline_code = argv[2];
        } else if (strcmp_u(argv[1], "--version") == 0 || strcmp_u(argv[1], "-v") == 0) {
            puts("Yolish v2.48 (Exploidus Native Runtime)\n");
            return 0;
        } else if (strcmp_u(argv[1], "--help") == 0 || strcmp_u(argv[1], "-h") == 0) {
            puts("Usage: ys <script.y> | ys -e '<code>'\n");
            puts("Yolish - The Reactive Capability Language of Exploidus OS\n");
            return 0;
        } else if (argv[1][0] != '-') {
            script = argv[1];
        }
    }

    /* Fallback: Read script path from /ys_run or default to /hello.y */
    if (!script && !inline_code) {
        int pfd = open("/ys_run", 0);
        if (pfd >= 0) {
            int n = (int)read(pfd, path_buf, sizeof(path_buf) - 1);
            if (n > 0) {
                while (n > 0 && (path_buf[n-1]=='\n'||path_buf[n-1]=='\r')) n--;
                path_buf[n] = 0;
                script = path_buf;
            }
            close(pfd);
        }
    }

    if (!script && !inline_code) {
        script = "/hello.y";
    }

    int len = 0;
    if (inline_code) {
        int n = 0;
        while (inline_code[n] && n < (int)sizeof(src_buf) - 1) {
            src_buf[n] = inline_code[n];
            n++;
        }
        src_buf[n] = 0;
        len = n;
    } else {
        len = read_file(script);
        if (len < 0) {
            puts("[YS] cannot open: ");
            puts(script);
            puts("\n");
            return 1;
        }
    }

    Lexer l;
    lex_init(&l, src_buf, len);
    Node *prog = parse_program(&l);
    Env *env = env_new(0);
    eval_program(prog, env);
    return 0;
}
