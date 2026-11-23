#include "esphome_api.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <time.h>

#define MAX_CLIENTS   3
#define RX_CAP        1024        /* light commands are ~40 bytes; anything this large is not for us */
#define LIGHT_KEY     0x6c696768u /* fixed entity key ("ligh") */
#define API_MINOR     14          /* 1.15+ makes clients ask for DeviceCapabilities, which a light has no use for */
#ifndef HL_ESPH_BIND_ADDR
#define HL_ESPH_BIND_ADDR INADDR_ANY   /* host tests set INADDR_LOOPBACK */
#endif

/* message ids, api.proto */
enum {
    HELLO_REQ = 1, HELLO_RESP = 2, AUTH_REQ = 3, DISCONNECT_REQ = 5, DISCONNECT_RESP = 6,
    PING_REQ = 7, PING_RESP = 8, DEVICE_INFO_REQ = 9, DEVICE_INFO_RESP = 10, LIST_ENTITIES_REQ = 11,
    LIST_ENTITIES_LIGHT = 15, LIST_ENTITIES_DONE = 19, SUBSCRIBE_STATES_REQ = 20, LIGHT_STATE = 24,
    LIGHT_COMMAND_REQ = 32,
};
#define COLOR_MODE_BRIGHTNESS 3

typedef struct {
    int      fd;
    bool     subscribed;
    int64_t  last_rx_ms;     /* monotonic; for the idle timeout and eviction */
    size_t   rx_len;
    uint8_t  rx[RX_CAP];
} client_t;

static const hl_esph_ops_t *s_ops;
static client_t s_cl[MAX_CLIENTS];

static void logf_(const char *fmt, ...)
{
    if (!s_ops || !s_ops->log) return;
    char b[160]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    s_ops->log(b);
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Never spin on a failing socket call: wait one poll period instead. */
static void backoff(int ms)
{
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    select(0, NULL, NULL, NULL, &tv);
}

/* ---- protobuf writer ---------------------------------------------------- */
typedef struct { uint8_t *p; size_t n, cap; bool overflow; } pb_t;

static void pb_byte(pb_t *b, uint8_t v) { if (b->n < b->cap) b->p[b->n++] = v; else b->overflow = true; }
static void pb_varint(pb_t *b, uint64_t v) { do { uint8_t c = v & 0x7f; v >>= 7; pb_byte(b, c | (v ? 0x80 : 0)); } while (v); }
static void pb_tag(pb_t *b, int field, int wt) { pb_varint(b, ((uint32_t)field << 3) | wt); }
static void pb_u32(pb_t *b, int f, uint32_t v) { if (v) { pb_tag(b, f, 0); pb_varint(b, v); } }
static void pb_bool(pb_t *b, int f, bool v) { if (v) { pb_tag(b, f, 0); pb_byte(b, 1); } }
static void pb_fixed32(pb_t *b, int f, uint32_t v) { pb_tag(b, f, 5); for (int i = 0; i < 4; i++) pb_byte(b, (v >> (8 * i)) & 0xff); }
static void pb_float(pb_t *b, int f, float v) { uint32_t u; memcpy(&u, &v, 4); if (u) pb_fixed32(b, f, u); }
static void pb_str(pb_t *b, int f, const char *s)
{
    size_t l = s ? strlen(s) : 0;
    pb_tag(b, f, 2); pb_varint(b, l);
    for (size_t i = 0; i < l; i++) pb_byte(b, (uint8_t)s[i]);
}

/* ---- protobuf reader ---------------------------------------------------- */
typedef struct { const uint8_t *p, *end; bool bad; } rd_t;

static uint64_t rd_varint(rd_t *r)
{
    uint64_t v = 0;
    for (int s = 0; s < 64; s += 7) {
        if (r->p >= r->end) { r->bad = true; return 0; }
        uint8_t c = *r->p++;
        v |= (uint64_t)(c & 0x7f) << s;
        if (!(c & 0x80)) return v;
    }
    r->bad = true; return 0;
}
static uint32_t rd_fixed32(rd_t *r)
{
    if (r->end - r->p < 4) { r->bad = true; return 0; }
    uint32_t v = r->p[0] | (r->p[1] << 8) | (r->p[2] << 16) | ((uint32_t)r->p[3] << 24);
    r->p += 4; return v;
}
static void rd_skip(rd_t *r, int wt)
{
    switch (wt) {
    case 0: rd_varint(r); break;
    case 1: if (r->end - r->p < 8) r->bad = true; else r->p += 8; break;
    case 2: { uint64_t l = rd_varint(r); if (!r->bad && (uint64_t)(r->end - r->p) >= l) r->p += l; else r->bad = true; break; }
    case 5: rd_fixed32(r); break;
    default: r->bad = true;
    }
}

/* ---- framing ------------------------------------------------------------ */
static void drop(client_t *c)
{
    if (c->fd >= 0) { close(c->fd); logf_("client %d disconnected", c->fd); }
    c->fd = -1; c->subscribed = false; c->rx_len = 0;
}

static bool send_msg(client_t *c, int type, const pb_t *payload)
{
    uint8_t hdr[12]; pb_t h = { hdr, 0, sizeof hdr, false };
    pb_byte(&h, 0x00);
    pb_varint(&h, payload ? payload->n : 0);
    pb_varint(&h, (uint32_t)type);
    uint8_t out[sizeof hdr + 512];
    size_t n = payload ? payload->n : 0;
    if (n > 512 || (payload && payload->overflow)) return false;
    memcpy(out, hdr, h.n);
    if (n) memcpy(out + h.n, payload->p, n);
    size_t total = h.n + n, off = 0;
    while (off < total) {
        ssize_t w = send(c->fd, out + off, total - off, 0);
        if (w <= 0) { if (w < 0 && errno == EINTR) continue; drop(c); return false; }
        off += (size_t)w;
    }
    return true;
}

/* ---- messages ----------------------------------------------------------- */
static void send_light_state(client_t *c, bool on, float bri)
{
    uint8_t buf[64]; pb_t b = { buf, 0, sizeof buf, false };
    pb_fixed32(&b, 1, LIGHT_KEY);
    pb_bool(&b, 2, on);
    pb_float(&b, 3, bri);
    pb_u32(&b, 11, COLOR_MODE_BRIGHTNESS);
    send_msg(c, LIGHT_STATE, &b);
}

static void handle(client_t *c, int type, const uint8_t *p, size_t len)
{
    uint8_t buf[512]; pb_t b = { buf, 0, sizeof buf, false };
    switch (type) {
    case HELLO_REQ:
        pb_u32(&b, 1, 1);
        pb_u32(&b, 2, API_MINOR);
        pb_str(&b, 3, "hdmilight-v3");
        pb_str(&b, 4, s_ops->name);
        send_msg(c, HELLO_RESP, &b);
        break;
    case AUTH_REQ:          /* no password: clients do not wait for a reply */
        break;
    case DISCONNECT_REQ:
        send_msg(c, DISCONNECT_RESP, NULL);
        drop(c);
        break;
    case PING_REQ:
        send_msg(c, PING_RESP, NULL);
        break;
    case DEVICE_INFO_REQ:
        pb_str(&b, 2, s_ops->name);
        pb_str(&b, 3, s_ops->mac);
        pb_str(&b, 4, s_ops->version);
        pb_str(&b, 5, s_ops->compile_time);
        pb_str(&b, 6, s_ops->model);
        pb_u32(&b, 10, s_ops->webserver_port);
        pb_str(&b, 12, s_ops->manufacturer);
        pb_str(&b, 13, s_ops->friendly_name);
        send_msg(c, DEVICE_INFO_RESP, &b);
        break;
    case LIST_ENTITIES_REQ:
        pb_str(&b, 1, "light");
        pb_fixed32(&b, 2, LIGHT_KEY);
        pb_str(&b, 3, "");                       /* empty: HA names the entity after the device */
        pb_u32(&b, 12, COLOR_MODE_BRIGHTNESS);   /* repeated enum, one entry */
        pb_str(&b, 14, "mdi:television-ambient-light");
        send_msg(c, LIST_ENTITIES_LIGHT, &b);
        if (c->fd >= 0) send_msg(c, LIST_ENTITIES_DONE, NULL);
        break;
    case SUBSCRIBE_STATES_REQ: {
        c->subscribed = true;
        bool on; float bri; s_ops->get(s_ops->ctx, &on, &bri);
        send_light_state(c, on, bri);
        break;
    }
    case LIGHT_COMMAND_REQ: {
        rd_t r = { p, p + len, false };
        uint32_t key = 0; bool has_state = false, state = false, has_bri = false; float bri = 0;
        while (r.p < r.end && !r.bad) {
            uint64_t tag = rd_varint(&r); int f = (int)(tag >> 3), wt = (int)(tag & 7);
            if (f == 1 && wt == 5) key = rd_fixed32(&r);
            else if (f == 2 && wt == 0) has_state = rd_varint(&r) != 0;
            else if (f == 3 && wt == 0) state = rd_varint(&r) != 0;
            else if (f == 4 && wt == 0) has_bri = rd_varint(&r) != 0;
            else if (f == 5 && wt == 5) { uint32_t u = rd_fixed32(&r); memcpy(&bri, &u, 4); }
            else rd_skip(&r, wt);
        }
        if (r.bad || key != LIGHT_KEY) { logf_("ignored light command (key %08x%s)", (unsigned)key, r.bad ? ", malformed" : ""); break; }
        if (has_bri) bri = bri < 0 ? 0 : bri > 1 ? 1 : bri;
        logf_("light command:%s%s%s", has_state ? (state ? " on" : " off") : "", has_bri ? " brightness" : "", (!has_state && !has_bri) ? " (nothing)" : "");
        s_ops->set(s_ops->ctx, has_state, state, has_bri, bri);
        break;   /* the poll loop pushes the new state to every subscriber */
    }
    default:   /* subscriptions to HA services/states/logs, time, etc.: nothing to do */
        break;
    }
}

/* Parse every complete frame in the client's buffer. */
static void process_rx(client_t *c)
{
    size_t off = 0;
    while (c->fd >= 0 && off < c->rx_len) {
        rd_t r = { c->rx + off, c->rx + c->rx_len, false };
        if (*r.p++ != 0x00) { logf_("client %d: not a plaintext frame (encryption?), closing", c->fd); drop(c); return; }
        uint64_t len = rd_varint(&r), type = r.bad ? 0 : rd_varint(&r);
        if (r.bad) break;                                   /* header incomplete */
        if (len > RX_CAP - 16) { logf_("client %d: frame of %u bytes, closing", c->fd, (unsigned)len); drop(c); return; }
        if ((uint64_t)(r.end - r.p) < len) break;          /* payload incomplete */
        handle(c, (int)type, r.p, (size_t)len);
        off = (size_t)(r.p - c->rx) + (size_t)len;
    }
    if (c->fd < 0) return;
    if (off) { memmove(c->rx, c->rx + off, c->rx_len - off); c->rx_len -= off; }
}

int hl_esph_serve(const hl_esph_ops_t *ops, uint16_t port, int poll_ms)
{
    s_ops = ops;
    for (int i = 0; i < MAX_CLIENTS; i++) s_cl[i].fd = -1;
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) { logf_("socket: %d", errno); return -1; }
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(HL_ESPH_BIND_ADDR) };
    if (bind(ls, (struct sockaddr *)&a, sizeof a) < 0 || listen(ls, 2) < 0) { logf_("bind/listen on %u: %d", port, errno); close(ls); return -1; }
    logf_("ESPHome API listening on port %u", port);

    const int idle_ms = ops->idle_timeout_ms > 0 ? ops->idle_timeout_ms : 90000;
    bool last_on = false; float last_bri = -1;
    for (;;) {
        fd_set rs; FD_ZERO(&rs); FD_SET(ls, &rs); int maxfd = ls;
        for (int i = 0; i < MAX_CLIENTS; i++) if (s_cl[i].fd >= 0) { FD_SET(s_cl[i].fd, &rs); if (s_cl[i].fd > maxfd) maxfd = s_cl[i].fd; }
        struct timeval tv = { poll_ms / 1000, (poll_ms % 1000) * 1000 };
        int n = select(maxfd + 1, &rs, NULL, NULL, &tv);
        if (n < 0) {
            if (errno != EINTR) { logf_("select failed (%d), backing off", errno); backoff(poll_ms); }
            continue;
        }
        int64_t now = now_ms();

        if (n > 0 && FD_ISSET(ls, &rs)) {
            int fd = accept(ls, NULL, NULL);
            if (fd < 0) {
                /* out of sockets/memory: lwIP drops the pending connection; elsewhere it may stay queued */
                logf_("accept failed (%d), backing off", errno);
                backoff(poll_ms);
            } else {
                client_t *slot = NULL;
                for (int i = 0; i < MAX_CLIENTS; i++) if (s_cl[i].fd < 0) { slot = &s_cl[i]; break; }
                if (!slot) {   /* full: make room by dropping the client we heard from least recently */
                    slot = &s_cl[0];
                    for (int i = 1; i < MAX_CLIENTS; i++) if (s_cl[i].last_rx_ms < slot->last_rx_ms) slot = &s_cl[i];
                    logf_("all slots taken, evicting client %d (silent %d ms)", slot->fd, (int)(now - slot->last_rx_ms));
                    drop(slot);
                }
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                struct timeval st = { 2, 0 };
                setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &st, sizeof st);
                slot->fd = fd; slot->subscribed = false; slot->rx_len = 0; slot->last_rx_ms = now;
                logf_("client %d connected", fd);
            }
        }
        for (int i = 0; n > 0 && i < MAX_CLIENTS; i++) {
            client_t *c = &s_cl[i];
            if (c->fd < 0 || !FD_ISSET(c->fd, &rs)) continue;
            ssize_t r = recv(c->fd, c->rx + c->rx_len, RX_CAP - c->rx_len, 0);
            if (r <= 0) { drop(c); continue; }
            c->rx_len += (size_t)r;
            c->last_rx_ms = now;
            process_rx(c);
            if (c->fd >= 0 && c->rx_len == RX_CAP) { logf_("client %d: rx buffer full, closing", c->fd); drop(c); }
        }

        /* a connection left half-open (Wi-Fi drop, client crash) would otherwise hold its slot forever */
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (s_cl[i].fd >= 0 && now - s_cl[i].last_rx_ms > idle_ms) {
                logf_("client %d silent for %d ms, dropping", s_cl[i].fd, (int)(now - s_cl[i].last_rx_ms));
                drop(&s_cl[i]);
            }

        /* push state changes (from HA, the web UI, anywhere) to subscribers */
        bool on; float bri; ops->get(ops->ctx, &on, &bri);
        if (on != last_on || bri != last_bri) {
            last_on = on; last_bri = bri;
            for (int i = 0; i < MAX_CLIENTS; i++) if (s_cl[i].fd >= 0 && s_cl[i].subscribed) send_light_state(&s_cl[i], on, bri);
        }
    }
}
