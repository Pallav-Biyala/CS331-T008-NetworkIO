/*
 * Echo app assumptions:
 * - --msg-size is a positive value no larger than --buf-size, so a partial
 *   frame fits in the core carry slot. core_config_default/core_parse_flags
 *   are expected to enforce the positive-value rule.
 * - When a read completes frames from both carry and the new read, the
 *   io_get_buf() buffer must fit all complete bytes. The app checks its
 *   reported capacity because io.h does not guarantee this for every
 *   possible --msg-size/--buf-size combination.
 * - Section 5.4's [SETUP] wire-rate estimates are benchmark-environment
 *   inputs; framing uses the configured --msg-size and does not encode a
 *   link-speed assumption.
 * - io_read()/io_write() return a negative errno without starting a request;
 *   an accepted request is completed only by its callback, as specified by
 *   io/io.h. Read data is copied before io_get_buf() because that call may
 *   invalidate the callback's read buffer.
 */
#include "../core/core.h"
#include "../io/io.h"

#include <string.h>

struct echo_conn {
    size_t pending_frames;
};

static struct echo_conn *echo_state(conn_t *c)
{
    return (struct echo_conn *)c->app_priv;
}

static void echo_fail(conn_t *c)
{
    CORE_STAT_INC(errors);
    io_close(c);
}

static void echo_read_again(conn_t *c)
{
    int rc = io_read(c, g_cfg->buf_size);
    if (rc < 0)
        echo_fail(c);
}

static int append_carry(conn_t *c, const void *data, size_t len)
{
    if (len == 0)
        return 0;
    return core_carry_append(c, data, len);
}

int app_init(const core_config_t *cfg)
{
    (void)cfg;
    return 0;
}

void app_shutdown(void)
{
}

void on_accept(conn_t *c)
{
    echo_read_again(c);
}

void on_read_done(conn_t *c, uint8_t *buf, ssize_t n)
{
    size_t frame_size = (size_t)g_cfg->msg_size;
    size_t read_len;
    size_t carry_len;
    size_t total_len;
    size_t complete_len;
    size_t trailing_len;

    if (n == 0) {
        core_carry_clear(c);
        io_close(c);
        return;
    }
    if (n < 0 || frame_size == 0 || (size_t)n > g_cfg->buf_size) {
        echo_fail(c);
        return;
    }

    read_len = (size_t)n;
    carry_len = (size_t)c->carry_len;
    if (carry_len > g_cfg->buf_size || read_len > SIZE_MAX - carry_len) {
        echo_fail(c);
        return;
    }
    total_len = carry_len + read_len;
    complete_len = total_len - (total_len % frame_size);
    trailing_len = total_len - complete_len;

    if (carry_len == 0) {
        if (append_carry(c, buf + complete_len, trailing_len) < 0) {
            echo_fail(c);
            return;
        }
        if (complete_len == 0) {
            echo_read_again(c);
            return;
        }

        echo_state(c)->pending_frames = complete_len / frame_size;
        if (io_write(c, buf, complete_len) < 0) {
            echo_state(c)->pending_frames = 0;
            echo_fail(c);
        }
        return;
    }

    if (complete_len == 0) {
        if (append_carry(c, buf, read_len) < 0) {
            echo_fail(c);
            return;
        }
        echo_read_again(c);
        return;
    }

    /*
     * The read buffer may be the same storage returned by io_get_buf(). Save
     * the new bytes first, then assemble complete frames in the write buffer.
     */
    {
        uint8_t *saved_read = core_slot_alloc();
        uint8_t *out;
        size_t out_cap = 0;
        size_t from_read;

        if (saved_read == NULL) {
            echo_fail(c);
            return;
        }
        memcpy(saved_read, buf, read_len);

        out = io_get_buf(c, &out_cap);
        if (out == NULL || complete_len > out_cap || complete_len < carry_len) {
            core_slot_free(saved_read);
            echo_fail(c);
            return;
        }

        memcpy(out, c->carry, carry_len);
        from_read = complete_len - carry_len;
        memcpy(out + carry_len, saved_read, from_read);

        /* The old carry is wholly consumed by at least one full frame. */
        core_carry_clear(c);
        if (append_carry(c, saved_read + from_read, trailing_len) < 0) {
            core_slot_free(saved_read);
            echo_fail(c);
            return;
        }
        core_slot_free(saved_read);

        echo_state(c)->pending_frames = complete_len / frame_size;
        if (io_write(c, out, complete_len) < 0) {
            echo_state(c)->pending_frames = 0;
            echo_fail(c);
        }
    }
}

void on_write_done(conn_t *c)
{
    struct echo_conn *state = echo_state(c);

    CORE_STAT_ADD(messages, state->pending_frames);
    state->pending_frames = 0;
    echo_read_again(c);
}

void on_file_read_done(conn_t *c, uint8_t *buf, ssize_t n)
{
    (void)c;
    (void)buf;
    (void)n;
}

void on_close(conn_t *c, io_close_reason_t reason)
{
    (void)c;
    (void)reason;
}

int main(int argc, char **argv)
{
    return core_main(argc, argv, "echo", 4096, sizeof(struct echo_conn));
}
