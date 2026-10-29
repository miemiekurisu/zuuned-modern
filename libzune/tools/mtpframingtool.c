/*
 * mtpframingtool - prove a streamed MTP send frames identically to a
 * memory send, without a Zune.
 *
 * mtp_send_object_from_file() used to malloc() the whole object, read the
 * whole file into it, and hand that buffer to the same code path an in-memory
 * send uses. That buffer is now gone: the data phase is streamed from the file
 * in 1 MB chunks. The claim that matters is therefore about WIRE FRAMING, not
 * about bytes read - a Zune must not be able to tell the two apart, because
 * this stack's device compatibility rests on chunk sizes, max-packet alignment
 * and zero-length-packet placement being exactly what they have always been.
 *
 * So: run both paths against a mock USB backend that records every bulk_out
 * write, and compare the recordings byte for byte. No device handle is opened
 * and no libusb call is made, which is exactly why this can run in a gate.
 *
 * Usage: mtpframingtool
 * Exits 0 when every check passes.
 *
 * Part of the native PTP/MTP stack that replaces vendored libmtp.
 */

#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* mkstemps() on the POSIX side */
#endif
#endif

#include "mtp.h"
#include "ptp.h"
#include "usb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <io.h>
#define CLOSE_FD(p) _close(p)
#else
#include <unistd.h>
#define CLOSE_FD(p) close(p)
#endif

/* Recorded bulk_out traffic for one send. */
typedef struct {
    uint8_t **data;   /* copied bytes, NULL for a zero-length packet */
    uint32_t *len;
    int n;
    int cap;
    int oom;
} wlog;

typedef struct {
    wlog log;
    uint64_t progress_calls;
    uint64_t progress_last;
    int progress_monotonic;
    int progress_reached_total;
    uint64_t progress_total_seen;
} mock_state;

static int checks_run = 0;
static int checks_failed = 0;

static void check(int ok, const char *what)
{
    checks_run++;
    if (!ok) {
        checks_failed++;
        printf("FAIL: %s\n", what);
    }
}

static void wlog_init(wlog *l)
{
    l->cap = 64;
    l->data = (uint8_t **)calloc((size_t)l->cap, sizeof(*l->data));
    l->len = (uint32_t *)calloc((size_t)l->cap, sizeof(*l->len));
    l->n = 0;
    l->oom = 0;
}

static void wlog_free(wlog *l)
{
    for (int i = 0; i < l->n; i++)
        free(l->data[i]);
    free(l->data);
    free(l->len);
    l->data = NULL;
    l->len = NULL;
    l->n = 0;
}

static void wlog_reset(wlog *l)
{
    for (int i = 0; i < l->n; i++) {
        free(l->data[i]);
        l->data[i] = NULL;
        l->len[i] = 0;
    }
    l->n = 0;
    l->oom = 0;
}

static void wlog_record(wlog *l, const uint8_t *data, uint32_t len)
{
    if (l->n == l->cap) {
        int cap = l->cap * 2;
        uint8_t **d = (uint8_t **)realloc(l->data, (size_t)cap * sizeof(*d));
        uint32_t *ln = (uint32_t *)realloc(l->len, (size_t)cap * sizeof(*ln));
        if (!d || !ln) {
            free(d);
            free(ln);
            l->oom = 1;
            return;
        }
        l->data = d;
        l->len = ln;
        l->cap = cap;
    }
    l->data[l->n] = NULL;
    if (len > 0) {
        l->data[l->n] = (uint8_t *)malloc(len);
        if (!l->data[l->n]) {
            l->oom = 1;
            return;
        }
        memcpy(l->data[l->n], data, len);
    }
    l->len[l->n] = len;
    l->n++;
}

static uint64_t wlog_total(const wlog *l)
{
    uint64_t t = 0;
    for (int i = 0; i < l->n; i++)
        t += l->len[i];
    return t;
}

/* ===================================================================
 * Mock USB backend
 * =================================================================== */

static int mock_open(zune_usb_handle_t *h, uint16_t vid, uint16_t pid)
{
    (void)h; (void)vid; (void)pid;
    return ZUNE_USB_OK;
}

static int mock_close(zune_usb_handle_t *h)
{
    (void)h;
    return ZUNE_USB_OK;
}

/* Every transaction the framing tool runs ends in a bare 12-byte OK
 * response; ptp_recv_response_timeout() checks container type and code only. */
static int mock_bulk_read(zune_usb_handle_t *h, uint8_t endpoint,
                          uint8_t *data, uint32_t length,
                          uint32_t *actual_length, uint32_t timeout_ms)
{
    (void)h; (void)endpoint; (void)timeout_ms;
    if (length < PTP_CONTAINER_HDR_SIZE)
        return ZUNE_USB_ERROR;
    data[0] = 12; data[1] = 0; data[2] = 0; data[3] = 0;
    data[4] = (uint8_t)(PTP_CT_RESPONSE & 0xFF);
    data[5] = (uint8_t)((PTP_CT_RESPONSE >> 8) & 0xFF);
    data[6] = (uint8_t)(PTP_RC_OK & 0xFF);
    data[7] = (uint8_t)((PTP_RC_OK >> 8) & 0xFF);
    data[8] = 0; data[9] = 0; data[10] = 0; data[11] = 0;
    *actual_length = PTP_CONTAINER_HDR_SIZE;
    return ZUNE_USB_OK;
}

static int mock_bulk_write(zune_usb_handle_t *h, uint8_t endpoint,
                           const uint8_t *data, uint32_t length,
                           uint32_t *actual_length, uint32_t timeout_ms)
{
    mock_state *m = (mock_state *)h->backend_data;
    (void)endpoint; (void)timeout_ms;
    wlog_record(&m->log, data, length);
    *actual_length = length;
    return m->log.oom ? ZUNE_USB_ERROR : ZUNE_USB_OK;
}

static int mock_clear_halt(zune_usb_handle_t *h, uint8_t endpoint)
{
    (void)h; (void)endpoint;
    return ZUNE_USB_OK;
}

static int mock_reset(zune_usb_handle_t *h)
{
    (void)h;
    return ZUNE_USB_OK;
}

static const zune_usb_backend_t mock_backend = {
    mock_open, mock_close, mock_bulk_read, mock_bulk_write,
    mock_clear_halt, mock_reset
};

static void on_progress(uint64_t sent, uint64_t total, void *userdata)
{
    mock_state *m = (mock_state *)userdata;

    m->progress_calls++;
    m->progress_total_seen = total;
    if (sent < m->progress_last)
        m->progress_monotonic = 0;
    m->progress_last = sent;
    if (sent == total)
        m->progress_reached_total = 1;
}

/* ===================================================================
 * Comparison
 * =================================================================== */

static int logs_equal(const wlog *a, const wlog *b)
{
    if (a->n != b->n)
        return 0;
    for (int i = 0; i < a->n; i++) {
        if (a->len[i] != b->len[i])
            return 0;
        if (a->len[i] == 0)
            continue;
        if (memcmp(a->data[i], b->data[i], a->len[i]) != 0)
            return 0;
    }
    return 1;
}

static void describe_log(const wlog *l)
{
    printf("  writes=%d bytes=%llu lengths=", l->n,
           (unsigned long long)wlog_total(l));
    for (int i = 0; i < l->n && i < 8; i++)
        printf("%u%s", l->len[i], i + 1 < l->n ? "," : "");
    if (l->n > 8)
        printf("...");
    printf("\n");
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ===================================================================
 * Case runner
 * =================================================================== */

static char g_tmp[4096];

/* Deterministic filler: every byte depends on its absolute offset, so a
 * chunk that was read twice, skipped, or handed out from the wrong position
 * changes the recorded stream instead of looking the same. */
static void fill_pattern(uint8_t *buf, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        buf[i] = (uint8_t)((i * 31u + (i >> 8) * 7u + 0x5Au) & 0xFFu);
}

static int make_tmp_file(const uint8_t *buf, uint32_t size)
{
    const char *dir = getenv("TEMP");
    if (!dir || !dir[0]) dir = getenv("TMP");
    if (!dir || !dir[0]) dir = ".";
#ifdef _WIN32
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    snprintf(g_tmp, sizeof(g_tmp), "%s%czuuned_framing_XXXXXX.bin", dir, sep);
    int fd = mkstemps(g_tmp, 4);
    if (fd < 0) {
        printf("FAIL: cannot make a temp file\n");
        return -1;
    }
    CLOSE_FD(fd);

    FILE *fp = fopen(g_tmp, "wb");
    if (!fp) {
        printf("FAIL: cannot write %s\n", g_tmp);
        remove(g_tmp);
        return -1;
    }
    if (size > 0 && fwrite(buf, 1, size, fp) != size) {
        printf("FAIL: short write to %s\n", g_tmp);
        fclose(fp);
        remove(g_tmp);
        return -1;
    }
    fclose(fp);
    return 0;
}

static void run_case(ptp_session_t *s, zune_usb_handle_t *h, mock_state *mem,
                     mock_state *mfl, const uint8_t *buf, uint32_t size,
                     uint16_t maxpkt, int split, char *tag)
{
    uint16_t resp = 0;
    int mem_rc, file_rc;

    snprintf(tag, 96, "size=%u maxpkt=%u split=%d", size, maxpkt, split);

    h->bulk_out_maxpacket = maxpkt;
    s->split_header_data = split;

    if (make_tmp_file(buf, size) != 0) {
        checks_run++;
        checks_failed++;
        return;
    }

    /* ---- memory path ---- */
    wlog_reset(&mem->log);
    mem->progress_calls = 0;
    mem->progress_last = 0;
    mem->progress_monotonic = 1;
    mem->progress_reached_total = 0;
    mem->progress_total_seen = 0;
    h->backend_data = mem;
    s->transaction_id = 0;   /* both runs get transaction 1 */
    mem_rc = ptp_transaction(s, PTP_OC_SendObject, NULL, 0, PTP_DP_SENDDATA,
                             (uint8_t *)buf, (uint64_t)size,
                             NULL, NULL, &resp, NULL, NULL);

    /* ---- streamed file path ---- */
    wlog_reset(&mfl->log);
    mfl->progress_calls = 0;
    mfl->progress_last = 0;
    mfl->progress_monotonic = 1;
    mfl->progress_reached_total = 0;
    mfl->progress_total_seen = 0;
    h->backend_data = mfl;
    s->transaction_id = 0;
    FILE *fp = fopen(g_tmp, "rb");
    if (!fp) {
        printf("FAIL: cannot reopen %s\n", g_tmp);
        remove(g_tmp);
        checks_run++;
        checks_failed++;
        return;
    }
    file_rc = ptp_transaction_send_file(s, PTP_OC_SendObject, NULL, 0, fp,
                                       (uint64_t)size, on_progress, mfl,
                                       &resp);
    fclose(fp);
    remove(g_tmp);

    check(mem_rc == 0, "memory path transaction succeeded");
    check(file_rc == 0, "file path transaction succeeded");
    if (mem_rc != 0 || file_rc != 0) return;

    if (!logs_equal(&mem->log, &mfl->log)) {
        printf("  framing differs (%s)\n", tag);
        printf("  memory:"); describe_log(&mem->log);
        printf("  file:  "); describe_log(&mfl->log);
    }
    check(logs_equal(&mem->log, &mfl->log),
          "command and data framing identical to the memory path");

    /*
     * Write 0 is the command container, so the data phase starts at 1, and
     * the first pure-payload write is at 2 whether or not the header went out
     * on its own. The rules below are the ones this stack had before any of
     * this existed; the streamed path has to reproduce all of them.
     */
    /*
     * Split mode always yields at least three writes: command, the lone
     * 12-byte data header, then a payload chunk. Combined mode folds the
     * header into the first write, and that write keeps the entire payload
     * whenever the container is short enough not to be trimmed down to a
     * max-packet boundary, so two writes there is a complete data phase and
     * not a truncation. The requirement therefore depends on the mode.
     */
    int minimum = split ? 3 : 2;
    check(mfl->log.n >= minimum, "command and the whole data phase went out");
    if (mfl->log.n < minimum) return;

    uint64_t phase_bytes = 0;
    for (int i = 1; i < mfl->log.n; i++)
        phase_bytes += mfl->log.len[i];
    check(phase_bytes == (uint64_t)size + PTP_CONTAINER_HDR_SIZE,
          "data phase is the header plus exactly send_len bytes");

    check(mfl->log.len[0] == PTP_CONTAINER_HDR_SIZE &&
          mfl->log.data[0][4] == PTP_CT_COMMAND,
          "command container precedes the data phase");

    if (split) {
        check(mfl->log.len[1] == PTP_CONTAINER_HDR_SIZE,
              "split mode sends the 12-byte data header alone");
        check(le32(mfl->log.data[1]) ==
                  (uint32_t)(size + PTP_CONTAINER_HDR_SIZE),
              "data header advertises the total container length");
        check(mfl->log.data[1][4] == PTP_CT_DATA,
              "data header is a DATA container");
        check(mfl->log.data[1][6] == (uint8_t)(PTP_OC_SendObject & 0xFF),
              "data header carries the SendObject opcode");
    } else {
        check(mfl->log.len[1] > PTP_CONTAINER_HDR_SIZE,
              "combined mode puts the header and payload in one write");
        check(le32(mfl->log.data[1]) ==
                  (uint32_t)(size + PTP_CONTAINER_HDR_SIZE),
              "combined header advertises the total container length");
        check(mfl->log.data[1][4] == PTP_CT_DATA,
              "combined header is a DATA container");
    }

    int zlp = mfl->log.len[mfl->log.n - 1] == 0;
    if (split)
        check(zlp == (size > 0 && (size % maxpkt) == 0),
              "split mode puts the ZLP on the payload boundary");
    else
        check(zlp == ((size + PTP_CONTAINER_HDR_SIZE) % maxpkt == 0),
              "combined mode puts the ZLP on the container boundary");

    /* A payload chunk is either a whole block, an aligned remainder, or the
     * final short packet. Anything larger and unaligned re-frames the pipe. */
    int aligned = 1;
    for (int i = 2; aligned && i < mfl->log.n; i++) {
        uint32_t l = mfl->log.len[i];
        if (l > (uint32_t)maxpkt && (l % (uint32_t)maxpkt) != 0)
            aligned = 0;
    }
    check(aligned, "every payload chunk is block-sized or max-packet aligned");

    check(mfl->progress_calls >= 1, "streamed send reported progress");
    check(mfl->progress_monotonic, "progress never went backwards");
    check(mfl->progress_reached_total, "progress reached the total");
    check(mfl->progress_total_seen == size, "progress total is the object size");
}

/* ===================================================================
 * Main
 * =================================================================== */

int main(void)
{
    /* Sizes chosen around every boundary the chunk loop can see: smaller
     * than a packet, exactly a packet, straddling one, one whole block,
     * a block plus an unaligned remainder (which is what makes the loop
     * align a chunk and then continue), and several blocks. */
    static const uint32_t sizes[] = {
        1u, 11u, 12u, 63u, 64u, 65u, 511u, 512u, 513u, 1000u, 1024u,
        65536u, 66048u, 1048575u, 1048576u, 1048577u, 1049576u,
        2097152u, 2097663u, 2098176u
    };
    static const uint16_t maxpkts[] = { 512, 64 };
    const uint32_t max_size = 2098176u;

    uint8_t *buf = (uint8_t *)malloc(max_size);
    mock_state mem;
    mock_state mfl;
    zune_usb_handle_t h;
    ptp_session_t s;
    char tag[96];

    if (!buf) {
        printf("FAIL: cannot allocate the test payload\n");
        return 1;
    }
    fill_pattern(buf, max_size);

    memset(&mem, 0, sizeof(mem));
    memset(&mfl, 0, sizeof(mfl));
    wlog_init(&mem.log);
    wlog_init(&mfl.log);

    memset(&h, 0, sizeof(h));
    h.backend = &mock_backend;
    h.bulk_out_ep = 0x02;
    h.bulk_in_ep = 0x86;
    h.bulk_out_maxpacket = 512;
    h.bulk_in_maxpacket = 512;
    h.backend_data = &mem;

    ptp_session_init(&s, &h);
    s.abort_flag = NULL;

    for (int split = 1; split >= 0; split--) {
        for (unsigned p = 0; p < sizeof(maxpkts) / sizeof(maxpkts[0]); p++) {
            for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
                run_case(&s, &h, &mem, &mfl, buf, sizes[i], maxpkts[p],
                         split, tag);
                if (checks_failed)
                    printf("  ^^ in case %s\n", tag);
            }
        }
    }

    /* The MTP layer in front of the data phase: an empty object is refused
     * rather than sent, because a 12-byte container leaves the device waiting
     * for a data phase with nothing in it. */
    checks_run++;
    if (mtp_send_object_from_file_progress(&s, g_tmp, 0, NULL, NULL) == 0) {
        checks_failed++;
        printf("FAIL: a 0-byte object was accepted\n");
    }
    checks_run++;
    if (mtp_send_object_from_file_progress(&s, "", 16, NULL, NULL) == 0) {
        checks_failed++;
        printf("FAIL: an empty path was accepted\n");
    }

    /* And one real object through the public MTP entry point, compared with
     * the memory path it replaced. */
    {
        const uint32_t n = 200000u;
        uint16_t resp = 0;
        uint8_t *one = (uint8_t *)buf;

        h.bulk_out_maxpacket = 512;
        s.split_header_data = 1;
        if (make_tmp_file(one, n) == 0) {
            wlog_reset(&mem.log);
            h.backend_data = &mem;
            s.transaction_id = 0;   /* both runs get transaction 1 */
            int rc_mem = ptp_transaction(&s, PTP_OC_SendObject, NULL, 0,
                                         PTP_DP_SENDDATA, one, (uint64_t)n,
                                         NULL, NULL, &resp, NULL, NULL);
            wlog_reset(&mfl.log);
            mfl.progress_calls = 0;
            mfl.progress_monotonic = 1;
            mfl.progress_reached_total = 0;
            mfl.progress_last = 0;
            mfl.progress_total_seen = 0;
            h.backend_data = &mfl;
            s.transaction_id = 0;
            int rc_file = mtp_send_object_from_file_progress(&s, g_tmp, n,
                                                             on_progress, &mfl);
            check(rc_mem == 0 && rc_file == 0,
                  "mtp_send_object_from_file_progress succeeded");
            check(logs_equal(&mem.log, &mfl.log),
                  "MTP entry point frames like the memory path");
            check(mfl.progress_calls >= 1 && mfl.progress_reached_total &&
                      mfl.progress_total_seen == n,
                  "MTP entry point reports real per-chunk progress");
            remove(g_tmp);
        }
    }

    wlog_free(&mem.log);
    wlog_free(&mfl.log);
    free(buf);

    printf("framing %d checks %d failures\n", checks_run, checks_failed);
    return checks_failed ? 1 : 0;
}
