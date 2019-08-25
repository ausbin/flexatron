#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "defs.h"

// TODO: Don't log directly to stderr

static const astro_err_t *wait_on_ack(astro_t *astro);

const astro_err_t *astro_gdb_ctx_setup(astro_t *astro) {
    astro->gdb_ctx.debugging = false;
    astro->gdb_ctx.break_next = false;
    astro->gdb_ctx.action = ACTION_WAIT;
    astro->gdb_ctx.len = 0;
    bzero(&astro->gdb_ctx.connbuf, sizeof astro->gdb_ctx.connbuf);
    astro->gdb_ctx.sockfd = -1;
    astro->gdb_ctx.connfd = -1;
    return NULL;
}

const astro_err_t *astro_host_gdb_server(astro_t *astro) {
    const astro_err_t *astro_err;

    astro->gdb_ctx.debugging = true;

    if ((astro->gdb_ctx.sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        astro_err = astro_perror(astro, "socket() for gdb server");
        goto failure;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ASTRO_GDB_PORT_NUMBER);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(astro->gdb_ctx.sockfd, (struct sockaddr *) &addr, sizeof addr) == -1) {
        astro_err = astro_perror(astro, "bind() for gdb server");
        goto failure;
    }

    if (listen(astro->gdb_ctx.sockfd, 8) == -1) {
        astro_err = astro_perror(astro, "listen() for gdb server");
        goto failure;
    }

    struct sockaddr_in client_addr;
    socklen_t client_addr_len;

    fprintf(stderr, "hosting gdb server on localhost:%d, waiting on gdb "
                    "client...\n", ASTRO_GDB_PORT_NUMBER);

    // For now, accept only one connection: the gdb client. This could
    // cause problems if you have firewall software or something
    // similarly stupid randomly connecting to open listening sockets on
    // your computer, but consider that a personal issue for now
    if ((astro->gdb_ctx.connfd = accept(astro->gdb_ctx.sockfd,
                                        (struct sockaddr *) &client_addr,
                                        &client_addr_len)) < 0) {
        astro_err = astro_perror(astro, "accept() for gdb server");
        goto failure;
    }

    fprintf(stderr, "gdb client connected, waiting on initial ack...\n");

    if (astro_err = wait_on_ack(astro))
        goto failure;

    fprintf(stderr, "received ack, proceeding...\n");

    return NULL;

    failure:
    if (astro->gdb_ctx.connfd >= 0 && close(astro->gdb_ctx.connfd) == -1)
        astro_err = astro_perror(astro, "close() connection socket");

    if (astro->gdb_ctx.sockfd >= 0 && close(astro->gdb_ctx.sockfd) == -1)
        astro_err = astro_perror(astro, "close() listening socket");

    return astro_err;
}

const astro_err_t *astro_close_gdb_server(astro_t *astro) {
    const astro_err_t *astro_err;

    if (close(astro->gdb_ctx.connfd) == -1) {
        astro_err = astro_perror(astro, "close() connection socket");
        goto failure;
    }

    if (close(astro->gdb_ctx.sockfd) == -1) {
        astro_err = astro_perror(astro, "close() listening socket");
        goto failure;
    }

    return NULL;

    failure:
    return astro_err;
}

void breakpoint_code_hook(uc_engine *uc, uint64_t address, uint32_t size,
                          void *user_data) {
    (void)uc;
    (void)address;
    (void)size;

    astro_t *astro = user_data;
    gdb_ctx_t *ctx = &astro->gdb_ctx;

    if (ctx->debugging && ctx->break_next) {
        ctx->break_next = false;

        const astro_err_t *astro_err;
        if (astro_err = wait_on_and_exec_command(astro))
            astro_sim_die(astro, astro_err);
    }
}

// Networking shit

typedef const astro_err_t *(*command_func_t)(astro_t *astro, const char *args,
                                             action_t *action_out);

static const astro_err_t *read_regs_command(astro_t *astro, const char *args,
                                            action_t *action_out);
static const astro_err_t *write_regs_command(astro_t *astro, const char *args,
                                             action_t *action_out);
static const astro_err_t *read_mem_command(astro_t *astro, const char *args,
                                           action_t *action_out);
static const astro_err_t *write_mem_command(astro_t *astro, const char *args,
                                            action_t *action_out);
static const astro_err_t *continue_command(astro_t *astro, const char *args,
                                           action_t *action_out);
static const astro_err_t *step_command(astro_t *astro, const char *args,
                                       action_t *action_out);
static const astro_err_t *reason_command(astro_t *astro, const char *args,
                                         action_t *action_out);

// Who needs a hashmap when you have memory
static const command_func_t commands[256] = {
    ['g'] = read_regs_command,
    ['G'] = write_regs_command,
    ['m'] = read_mem_command,
    ['M'] = write_mem_command,
    ['c'] = continue_command,
    ['s'] = step_command,
    ['?'] = reason_command,
};

static unsigned char calc_checksum(const char *buf,
                                   unsigned int len) {
    const unsigned char *ubuf = (const unsigned char *) buf;
    unsigned char checksum = 0;
    for (unsigned int i = 0; i < len; i++)
        checksum += ubuf[i];
    return checksum;
}

static unsigned char read_hex_char(char c) {
    if ('0' <= c && c <= '9') return c - '0';
    if ('a' <= c && c <= 'f') return c - 'a' + 10;
    if ('A' <= c && c <= 'F') return c - 'A' + 10;
    // TODO: handle this better
    return 0;
}

static unsigned char read_checksum(char *buf) {
    return read_hex_char(buf[0]) << 4 | read_hex_char(buf[1]);
}

static const astro_err_t *wait_on_ack(astro_t *astro) {
    const astro_err_t *astro_err = NULL;
    gdb_ctx_t *ctx = &astro->gdb_ctx;

    ssize_t n;
    char minibuf;
    n = read(ctx->connfd, &minibuf, sizeof minibuf);
    if (n < 0) {
        astro_err = astro_perror(astro, "gdb connection read");
        goto failure;
    } else if (n == 0) {
        // TODO: handle EOF
        astro_err = astro_errorf(astro, "i don't know how to handle eof");
        goto failure;
    }

    if (minibuf == '-') {
        astro_err = astro_errorf(astro, "i don't know how to handle nak yet");
        goto failure;
    } else if (minibuf != '+') {
        astro_err = astro_errorf(astro, "unknown non-ack \\x%02x", minibuf);
        goto failure;
    }

    failure:
    return astro_err;
}

static const astro_err_t *send_ack(astro_t *astro) {
    const astro_err_t *astro_err = NULL;
    gdb_ctx_t *ctx = &astro->gdb_ctx;

    char minibuf = '+';
    ssize_t n = write(ctx->connfd, &minibuf, 1);
    if (n < 0) {
        astro_err = astro_perror(astro, "gdb server: write");
        goto failure;
    }

    failure:
    return astro_err;
}

static const astro_err_t *read_packet(astro_t *astro) {
    const astro_err_t *astro_err = NULL;
    gdb_ctx_t *ctx = &astro->gdb_ctx;

    bool need_to_read = !ctx->len;

    while (1) {
        if (ctx->len == sizeof ctx->connbuf) {
            astro_err = astro_errorf(astro, "overflowed max packet size of %lu",
                                     sizeof ctx->connbuf);
            goto failure;
        }

        if (need_to_read) {
            ssize_t n = read(ctx->connfd, ctx->connbuf + ctx->len,
                             sizeof ctx->connbuf - ctx->len);
            if (n < 0) {
                astro_err = astro_perror(astro, "gdb connection read");
                goto failure;
            } else if (n == 0) {
                // TODO: handle EOF
                astro_err = astro_perror(astro, "i don't know how to handle eof");
                goto failure;
            }

            ctx->len += n;
            need_to_read = false;
        }

        // On the first read, check that it's legit
        if (*ctx->connbuf != '$') {
            astro_err = astro_errorf(astro, "stray data found. \\x%02x is not "
                                            "'$'", *ctx->connbuf);
            goto failure;
        }

        // Find the # in the request
        unsigned int i;
        for (i = 0; i < ctx->len && ctx->connbuf[i] != '#'; i++);

        // Don't have the end of the request yet, need more reads
        if (i + 2 >= ctx->len) {
            need_to_read = true;
            continue;
        }

        unsigned char our_checksum = calc_checksum(ctx->connbuf + 1, i - 1);
        unsigned char their_checksum = read_checksum(ctx->connbuf + i + 1);

        if (our_checksum != their_checksum) {
            astro_err = astro_errorf(astro, "checksums don't match. can't "
                                            "handle this yet");
            goto failure;
        }

        memcpy(ctx->argbuf, ctx->connbuf + 1, i - 1);
        ctx->argbuf[i - 1] = '\0';

        unsigned int packet_len = i + 3;
        if (packet_len < ctx->len) {
            memmove(ctx->connbuf, ctx->connbuf + packet_len,
                    ctx->len - packet_len);
        }

        ctx->len -= packet_len;

        break;
    }

    failure:
    return astro_err;
}

static const astro_err_t *send_response(astro_t *astro, const char *response) {
    const astro_err_t *astro_err = NULL;
    gdb_ctx_t *ctx = &astro->gdb_ctx;

    size_t response_len = strlen(response);
    size_t total_packet_len = response_len + strlen("$#xx");

    // +1 for null terminator written by sprintf()
    char *writebuf = malloc(total_packet_len + 1);

    if (!writebuf) {
        astro_err = astro_perror(astro, "gdb server: malloc");
        goto failure;
    }

    writebuf[0] = '$';
    memcpy(writebuf + 1, response, response_len);
    writebuf[1 + response_len] = '#';

    unsigned char checksum = calc_checksum(response, response_len);
    snprintf(writebuf + 1 + response_len + 1, 3, "%02x", checksum);

    ssize_t n = write(ctx->connfd, writebuf, total_packet_len);
    if (n < 0) {
        astro_err = astro_perror(astro, "gdb server: write");
        goto failure;
    }

    fprintf(stderr, "response: `%s'\n", response);

    failure:
    return astro_err;
}

static const astro_err_t *send_stopped_response(astro_t *astro) {
    // Always claim we stopped for no reason (signal # 0x00), which is a
    // complete lie
    // TODO: fix
    return send_response(astro, "S00");
}

const astro_err_t *wait_on_and_exec_command(astro_t *astro) {
    const astro_err_t *astro_err = NULL;
    gdb_ctx_t *ctx = &astro->gdb_ctx;

    // Must be a reason we stopped. Possibilities by decreasing priority:
    //  1. breakpoint (TODO)
    //  2. segfault
    //  3. stepping (step only)
    //  4. program finished
    if (ctx->action == ACTION_STEP || ctx->action == ACTION_CONTINUE) {
        if (ctx->action == ACTION_STEP) {
            // TODO: may not be correct. This only handles #3 above, what
            //       about segfaults?
            // Tell gdb that single-stepping is a hardware breakpoint
            if (astro_err = send_response(astro, "S05hwbreak:"))
                goto failure;
        } else if (ctx->action == ACTION_CONTINUE) {
            // TODO: do this
        }

        // We've sent a response, so now we want an ack
        if (astro_err = wait_on_ack(astro))
            goto failure;
    }

    ctx->action = ACTION_WAIT;

    while (ctx->action == ACTION_WAIT) {
        if (astro_err = read_packet(astro))
            goto failure;

        if (astro_err = send_ack(astro))
            goto failure;

        fprintf(stderr, "packet: `%s'\n", ctx->argbuf);

        unsigned char command = *ctx->argbuf;
        if (commands[command]) {
            commands[command](astro, ctx->argbuf + 1, &ctx->action);
        } else {
            // Per the spec, "for any command not supported by the stub, an empty
            // response (`$#00') should be returned."
            if (astro_err = send_response(astro, ""))
                goto failure;
        }

        // if the action is step or continue, we haven't responded yet
        // so we won't get an ack and shouldn't wait
        if (ctx->action != ACTION_STEP &&
                ctx->action != ACTION_CONTINUE &&
                (astro_err = wait_on_ack(astro)))
            goto failure;
    }

    ctx->break_next = ctx->action == ACTION_STEP;

    failure:
    return astro_err;
}

static const astro_err_t *gen_regs_response(astro_t *astro, uint64_t *regvals,
                                            size_t count,
                                            const char **response_out) {
    const astro_err_t *astro_err = NULL;

    // Assume each register is 8 bytes for now
    char *response = malloc(count * 8 * 2 + 1);
    if (!response) {
        astro_err = astro_perror(astro, "malloc g response");
        goto failure;
    }

    for (unsigned int i = 0; i < count; i++) {
        uint64_t n = regvals[i];
        // Godawful way to print in little endian, please don't tell my
        // family they wouldn't be able to handle it
        snprintf(response + i*8*2, 8*2+1, "%02lx%02lx%02lx%02lx%02lx%02lx%02lx%02lx",
                       n & 0xff,  n >> 8 & 0xff, n >> 16 & 0xff, n >> 24 & 0xff,
                 n >> 32 & 0xff, n >> 40 & 0xff, n >> 48 & 0xff, n >> 56 & 0xff);
    }

    *response_out = response;
    return NULL;

    failure:
    free(response);
    return astro_err;
}

static const astro_err_t *read_regs_command(astro_t *astro, const char *args,
                                            action_t *action_out) {
    (void)args;

    uc_err err;
    const astro_err_t *astro_err = NULL;

    // This order is from gdb/amd64-tdep.c in the gdb source code
    int regs[] = {
        UC_X86_REG_RAX, /* %rax */
        UC_X86_REG_RBX, /* %rbx */
        UC_X86_REG_RCX, /* %rcx */
        UC_X86_REG_RDX, /* %rdx */
        UC_X86_REG_RSI, /* %rsi */
        UC_X86_REG_RDI, /* %rdi */
        UC_X86_REG_RBP, /* %rbp */
        UC_X86_REG_RSP, /* %rsp */
        UC_X86_REG_R8,  /* %r8 */
        UC_X86_REG_R9,  /* %r9 */
        UC_X86_REG_R10, /* %r10 */
        UC_X86_REG_R11, /* %r11 */
        UC_X86_REG_R12, /* %r12 */
        UC_X86_REG_R13, /* %r13 */
        UC_X86_REG_R14, /* %r14 */
        UC_X86_REG_R15, /* %r15 */
        UC_X86_REG_RIP, /* %rip */
    };

    const size_t nregs = sizeof regs / sizeof *regs;
    uint64_t regvals[nregs];
    void *regval_ptrs[nregs];

    for (unsigned int i = 0; i < nregs; i++)
        regval_ptrs[i] = &regvals[i];

    if (err = uc_reg_read_batch(astro->uc, regs, regval_ptrs, nregs)) {
        astro_err = astro_uc_perror(astro, "uc_reg_read_batch", err);
        goto failure;
    }

    const char *response;
    if (astro_err = gen_regs_response(astro, regvals, nregs, &response))
        goto failure;

    if (astro_err = send_response(astro, response))
        goto failure;

    *action_out = ACTION_WAIT;

    failure:
    return astro_err;
}

static const astro_err_t *write_regs_command(astro_t *astro, const char *args,
                                             action_t *action_out) {
    (void)args;
    (void)action_out;

    return astro_errorf(astro, "can't write regs yet");

    //*action_out = ACTION_WAIT;
    //return NULL;
}

static const astro_err_t *read_mem_command(astro_t *astro, const char *args,
                                           action_t *action_out) {
    (void)astro;
    (void)args;

    *action_out = ACTION_WAIT;
    return NULL;
}

static const astro_err_t *write_mem_command(astro_t *astro, const char *args,
                                            action_t *action_out) {
    (void)args;
    (void)action_out;

    return astro_errorf(astro, "can't write memory yet");

    //*action_out = ACTION_WAIT;
    //return NULL;
}

static const astro_err_t *continue_command(astro_t *astro, const char *args,
                                           action_t *action_out) {
    (void)args;
    (void)action_out;

    return astro_errorf(astro, "can't continue yet");

    //*action_out = ACTION_CONTINUE;
    //return NULL;
}

static const astro_err_t *step_command(astro_t *astro, const char *args,
                                       action_t *action_out) {
    (void)astro;
    (void)args;

    *action_out = ACTION_STEP;
    return NULL;
}

static const astro_err_t *reason_command(astro_t *astro, const char *args,
                                         action_t *action_out) {
    (void)astro;
    (void)args;

    const astro_err_t *astro_err = NULL;

    // Send the status response now
    if (astro_err = send_stopped_response(astro))
        goto failure;

    *action_out = ACTION_WAIT;

    failure:
    return astro_err;
}