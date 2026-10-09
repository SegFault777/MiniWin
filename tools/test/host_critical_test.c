/* host_critical_test.c -- regression tests for the three "critical" findings of the
 * 2026-10-08 bug audit (docs: MiniWin 버그 감사 보고서):
 *   C-01  ATA status polling must time out instead of hanging the kernel forever
 *   C-02  a program that was only partly read from disk must never count as "loaded"
 *   C-03  a SYN-ACK only completes the handshake if its ACK acknowledges OUR SYN
 *         (plus: incoming TCP segments with a bad checksum are dropped)
 *
 * The real kernel headers (ata.h, fs.h, tcp.h) are compiled on the build host. Port I/O
 * (inb/outb/...) is replaced by a tiny fake ATA controller whose misbehaviour we can
 * dial in, and the fixed network-buffer addresses are mmap()ed as ordinary memory.
 *   tools/test/run_host_critical.sh                                                    */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>

/* ---- fake io.h: claim the include guard, then provide the same API ---- */
#define IO_H
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef int            i32;

enum { MODE_OK, MODE_BSY_FOREVER, MODE_FLOAT_FF, MODE_NO_DRQ, MODE_DRIVE_FAULT,
       MODE_READ_ERR_LBA, MODE_WRITE_ERR, MODE_FLUSH_HANG };
static int      ata_mode = MODE_OK;
static u32      ata_err_lba = 0xFFFFFFFFu;
#define DISK_SECTORS 4096
static u8       disk[DISK_SECTORS * 512];
static u32      cur_lba;
static int      cur_cmd;            /* 0 none, 0x20 read, 0x30 write */
static int      xfer_words;
static u8       wr_sector[512];
static int      flush_done;
static unsigned long io_polls;      /* every status read, to show the loops are bounded */

static u8 inb(u16 port) {
    if (port != 0x1F7) return 0xFF;                       /* COM1 line status etc: "ready" */
    io_polls++;
    switch (ata_mode) {
        case MODE_BSY_FOREVER: return 0x80;
        case MODE_FLOAT_FF:    return 0xFF;
        case MODE_NO_DRQ:      return 0x40;
        case MODE_DRIVE_FAULT: return 0x40 | 0x20;
        case MODE_FLUSH_HANG:  if (flush_done) return 0x80; /* fallthrough */
        default: break;
    }
    if (cur_cmd == 0x20 && ata_mode == MODE_READ_ERR_LBA && cur_lba == ata_err_lba) return 0x41; /* RDY|ERR */
    if (cur_cmd == 0x30 && ata_mode == MODE_WRITE_ERR) return 0x41;
    if (cur_cmd == 0x20 || cur_cmd == 0x30) return xfer_words < 256 ? 0x48 : 0x40; /* RDY|DRQ */
    return 0x40;
}
static void outb(u16 port, u8 val) {
    if (port == 0x1F7) {
        if (val == 0x20 || val == 0x30) { cur_cmd = val; xfer_words = 0; }
        else if (val == 0xE7) { cur_cmd = 0; flush_done = 1; }
    } else if (port == 0x1F3) cur_lba = (cur_lba & ~0xFFu) | val;
    else if (port == 0x1F4) cur_lba = (cur_lba & ~0xFF00u) | ((u32)val << 8);
    else if (port == 0x1F5) cur_lba = (cur_lba & ~0xFF0000u) | ((u32)val << 16);
}
static u16 inw(u16 port) {
    (void)port;
    u16 w = *(u16 *)&disk[cur_lba * 512 + xfer_words * 2];
    if (++xfer_words == 256) cur_cmd = 0;
    return w;
}
static void outw(u16 port, u16 val) {
    (void)port;
    *(u16 *)&wr_sector[xfer_words * 2] = val;
    if (++xfer_words == 256) { memcpy(&disk[cur_lba * 512], wr_sector, 512); cur_cmd = 0x31; flush_done = 0; }
}
static void outl(u16 p, u32 v) { (void)p; (void)v; }
static u32  inl(u16 p) { (void)p; return 0; }
static void io_wait(void) {}

#include "../../kernel/ata.h"
#include "../../kernel/fs.h"
#include "../../kernel/tcp.h"
#include "../../kernel/udp.h"

static int checks = 0, failures = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void on_alarm(int s) { (void)s; printf("FAIL: HANG (a bounded wait never returned)\n"); _exit(2); }

static void disk_reset(void) {
    memset(disk, 0, sizeof disk);
    ata_mode = MODE_OK; ata_err_lba = 0xFFFFFFFFu; cur_cmd = 0; flush_done = 0;
}

/* ------------------------------------------------------------------ C-01 */
static void test_ata_timeouts(void) {
    u8 buf[512], src[512];
    for (int i = 0; i < 512; i++) src[i] = (u8)(i * 7 + 1);

    disk_reset();
    CHECK(ata_write_sector(100, src) == 1, "healthy write succeeds");
    memset(buf, 0, sizeof buf);
    CHECK(ata_read_sector(100, buf) == 1 && memcmp(buf, src, 512) == 0, "healthy read returns the written data");

    const char *names[] = { "", "BSY forever", "floating bus 0xFF", "DRQ never set", "drive fault (DF)" };
    int modes[] = { 0, MODE_BSY_FOREVER, MODE_FLOAT_FF, MODE_NO_DRQ, MODE_DRIVE_FAULT };
    for (int k = 1; k < 5; k++) {
        disk_reset(); ata_mode = modes[k];
        io_polls = 0;
        CHECK(ata_read_sector(100, buf) == 0,  "read fails (not hangs) on: %s", names[k]);
        CHECK(ata_write_sector(100, src) == 0, "write fails (not hangs) on: %s", names[k]);
        CHECK(io_polls < 20u * ATA_POLL_LIMIT, "bounded polling on %s (%lu status reads)", names[k], io_polls);
    }

    disk_reset(); ata_mode = MODE_WRITE_ERR;
    CHECK(ata_write_sector(100, src) == 0, "write reports ERR status as failure");

    disk_reset(); ata_mode = MODE_FLUSH_HANG;
    CHECK(ata_write_sector(100, src) == 0, "write does not claim success when the cache flush never completes");

    disk_reset();
    CHECK(ata_write_sector(100, src) == 1, "drive works again after the faults are gone (no stuck state)");
}

/* ------------------------------------------------------------------ C-02 */
static void test_partial_program(void) {
    static u8 prog[5000], dst[PROG_MAX_BYTES], ref[PROG_MAX_BYTES];
    for (int i = 0; i < 5000; i++) prog[i] = (u8)(i * 13 + 5);

    disk_reset();
    CHECK(prog_save_slot(0, "TEST.MWP", prog, 5000, 0) == 1, "save a 5000-byte program");
    memset(dst, 0xEE, sizeof dst);
    CHECK(prog_load_slot(0, dst, PROG_MAX_BYTES) == 5000, "intact program loads in full");
    CHECK(memcmp(dst, prog, 5000) == 0, "loaded bytes match what was saved");

    /* one sector in the MIDDLE of the program fails to read: sector index 4 of 10 */
    ata_mode = MODE_READ_ERR_LBA; ata_err_lba = prog_slot_data_lba(0) + 4;
    memset(dst, 0xEE, sizeof dst);
    u32 got = prog_load_slot(0, dst, PROG_MAX_BYTES);
    CHECK(got == 0, "mid-program read error => load fails (got %u, old behaviour returned the partial count 2048)", got);

    /* the LAST sector fails */
    ata_err_lba = prog_slot_data_lba(0) + 9;
    CHECK(prog_load_slot(0, dst, PROG_MAX_BYTES) == 0, "last-sector read error => load fails");

    /* a destination that cannot hold the whole program is refused, not silently truncated */
    ata_mode = MODE_OK;
    CHECK(prog_load_slot(0, dst, 4096) == 0, "maxlen smaller than the program => refused, not truncated");

    /* empty slot */
    CHECK(prog_load_slot(1, dst, PROG_MAX_BYTES) == 0, "empty slot => 0");

    /* the header itself fails to read */
    ata_mode = MODE_READ_ERR_LBA; ata_err_lba = prog_slot_header_lba(0);
    CHECK(prog_load_slot(0, dst, PROG_MAX_BYTES) == 0, "header read error => 0");

    /* full-size program still fine */
    ata_mode = MODE_OK;
    for (u32 i = 0; i < PROG_MAX_BYTES; i++) ref[i] = (u8)(i ^ (i >> 8));
    CHECK(prog_save_slot(2, "BIG.MWP", ref, PROG_MAX_BYTES, 0) == 1, "save a max-size program");
    CHECK(prog_load_slot(2, dst, PROG_MAX_BYTES) == PROG_MAX_BYTES && memcmp(dst, ref, PROG_MAX_BYTES) == 0,
          "max-size program loads in full");
}

/* ------------------------------------------------------------------ C-03 */
static u16 test_csum(const u8 *seg, u16 len, u32 src, u32 dst) {      /* independent RFC 793/1071 implementation */
    u32 sum = 0;
    sum += (src >> 16) + (src & 0xFFFF);
    sum += (dst >> 16) + (dst & 0xFFFF);
    sum += 6 + len;
    for (u16 i = 0; i + 1 < len; i += 2) sum += (seg[i] << 8) | seg[i + 1];
    if (len & 1) sum += seg[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)~sum;
}

#define PEER_IP   0x0A000202u
#define MY_IP     0x0A00020Fu
#define PEER_PORT 80

/* ---- fake NIC: records frames, can be told to refuse them ---- */
static int fake_send_ok = 1;
static int sent_frames = 0;
static int fake_send(const u8 *frame, u16 len) { (void)frame; (void)len; if (!fake_send_ok) return 0; sent_frames++; return 1; }
static void arp_put(u32 ip, int resolved) {
    arp_cache_init();
    for (int i = 0; i < ARP_CACHE_SIZE; i++) arp_cache[i].state = ARP_ENTRY_EMPTY;
    if (resolved) { arp_cache[0].ip = ip; arp_cache[0].state = ARP_ENTRY_RESOLVED; for (int j = 0; j < 6; j++) arp_cache[0].mac[j] = (u8)(0x52 + j); }
}
static void net_setup(void) {
    nic.present = 1; nic.send = fake_send; for (int i = 0; i < 6; i++) nic.mac[i] = (u8)(0x02 + i);
    net_cfg.my_ip = MY_IP; net_cfg.netmask = 0xFFFFFF00u; net_cfg.gateway_ip = 0x0A000202u; net_cfg.ready = 1;
    fake_send_ok = 1; sent_frames = 0; ip_pending.pending = 0;
    arp_put(PEER_IP, 1);
}

static u8 seg_buf[1600];
static u16 build_seg(u8 flags, u32 seq, u32 ack, const u8 *data, u16 dlen, int with_mss, int good_csum) {
    u16 hl = with_mss ? 24 : 20;
    memset(seg_buf, 0, hl);
    seg_buf[0] = PEER_PORT >> 8; seg_buf[1] = PEER_PORT & 0xFF;
    seg_buf[2] = tcp_conn.local_port >> 8; seg_buf[3] = tcp_conn.local_port & 0xFF;
    for (int i = 0; i < 4; i++) { seg_buf[4 + i] = seq >> (24 - 8 * i); seg_buf[8 + i] = ack >> (24 - 8 * i); }
    seg_buf[12] = (u8)((hl / 4) << 4); seg_buf[13] = flags; seg_buf[14] = 0xFF; seg_buf[15] = 0xFF;
    if (with_mss) { seg_buf[20] = 2; seg_buf[21] = 4; seg_buf[22] = 0x05; seg_buf[23] = 0xB4; } /* MSS 1460 */
    if (dlen) memcpy(seg_buf + hl, data, dlen);
    u16 len = (u16)(hl + dlen);
    u16 c = test_csum(seg_buf, len, PEER_IP, MY_IP);
    if (!good_csum) c ^= 0x0101;
    seg_buf[16] = c >> 8; seg_buf[17] = c & 0xFF;
    return len;
}
static void deliver(u16 len) {
    ip_packet_t ip; memset(&ip, 0, sizeof ip);
    ip.proto = IP_PROTO_TCP; ip.src_ip = PEER_IP; ip.dst_ip = MY_IP;
    ip.payload = seg_buf; ip.payload_len = len;
    tcp_handle_packet(&ip);
}
static void new_conn(void) {
    net_setup();                              /* fake NIC accepts frames, next hop resolved */
    tcp_connect(PEER_IP, PEER_PORT);
}

static void test_tcp_synack(void) {
    /* correct SYN-ACK completes the handshake */
    new_conn();
    u32 good_ack = tcp_conn.snd_nxt;           /* ISN + 1 */
    CHECK(tcp_conn.state == TCP_SYN_SENT, "connect enters SYN_SENT");
    deliver(build_seg(TCP_FLAG_SYN | TCP_FLAG_ACK, 5000, good_ack, 0, 0, 1, 1));
    CHECK(tcp_conn.state == TCP_ESTABLISHED, "SYN-ACK with the right ACK => ESTABLISHED");
    CHECK(tcp_conn.rcv_nxt == 5001, "rcv_nxt = peer ISN + 1 (got %u)", tcp_conn.rcv_nxt);
    CHECK(tcp_conn.peer_mss == 1460, "MSS option parsed (got %u)", tcp_conn.peer_mss);

    /* forged / stale ACK numbers must NOT establish (each is relative to THIS connection's snd_nxt) */
    static const u32 deltas[] = { 1, (u32)-1, (u32)-2, 7, 0x80000000u, 0x7FFFFFFFu };
    for (unsigned k = 0; k < sizeof deltas / sizeof deltas[0]; k++) {
        new_conn();
        u32 ack = tcp_conn.snd_nxt + deltas[k];
        deliver(build_seg(TCP_FLAG_SYN | TCP_FLAG_ACK, 9000, ack, 0, 0, 1, 1));
        CHECK(tcp_conn.state == TCP_SYN_SENT, "SYN-ACK with ACK = snd_nxt%+d stays SYN_SENT (state %d)", (int)deltas[k], (int)tcp_conn.state);
        CHECK(tcp_conn.rcv_nxt == 0, "wrong-ACK SYN-ACK must not touch rcv_nxt (got %u)", tcp_conn.rcv_nxt);
        CHECK(tcp_conn.retx_pending == 1, "our SYN stays armed for retransmit after a bogus SYN-ACK");
    }
    {   /* ACK = 0 and ACK = 1: the lazy-forgery values */
        u32 lazy[] = { 0, 1 };
        for (int k = 0; k < 2; k++) {
            new_conn();
            deliver(build_seg(TCP_FLAG_SYN | TCP_FLAG_ACK, 9000, lazy[k], 0, 0, 1, 1));
            CHECK(tcp_conn.state == TCP_SYN_SENT, "SYN-ACK with ACK=%u stays SYN_SENT", lazy[k]);
        }
    }

    /* a stale SYN-ACK from an EARLIER attempt to the same server (old ISN) is rejected by a new attempt */
    new_conn(); u32 old_ack = tcp_conn.snd_nxt;
    tcp_conn.state = TCP_CLOSED;
    new_conn();
    CHECK(tcp_conn.snd_nxt != old_ack, "second attempt uses a fresh ISN");
    deliver(build_seg(TCP_FLAG_SYN | TCP_FLAG_ACK, 7000, old_ack, 0, 0, 1, 1));
    CHECK(tcp_conn.state == TCP_SYN_SENT, "stale SYN-ACK from the previous attempt is ignored");

    /* right ACK but corrupted checksum: dropped, then the genuine one still works */
    new_conn(); good_ack = tcp_conn.snd_nxt;
    deliver(build_seg(TCP_FLAG_SYN | TCP_FLAG_ACK, 8000, good_ack, 0, 0, 1, 0));
    CHECK(tcp_conn.state == TCP_SYN_SENT, "SYN-ACK with a bad checksum is dropped");
    deliver(build_seg(TCP_FLAG_SYN | TCP_FLAG_ACK, 8000, good_ack, 0, 0, 1, 1));
    CHECK(tcp_conn.state == TCP_ESTABLISHED, "the genuine SYN-ACK afterwards still establishes");

    /* established: data with bad checksum dropped, good checksum accepted */
    u32 before = tcp_conn.recv_len;
    deliver(build_seg(TCP_FLAG_ACK | TCP_FLAG_PSH, 8001, tcp_conn.snd_nxt, (const u8 *)"HELLO", 5, 0, 0));
    CHECK(tcp_conn.recv_len == before, "data segment with bad checksum is not accepted");
    deliver(build_seg(TCP_FLAG_ACK | TCP_FLAG_PSH, 8001, tcp_conn.snd_nxt, (const u8 *)"HELLO", 5, 0, 1));
    CHECK(tcp_conn.recv_len == before + 5 && memcmp(TCP_RECV_BUF + before, "HELLO", 5) == 0, "data segment with good checksum is accepted");

    /* odd-length payload checksum path */
    deliver(build_seg(TCP_FLAG_ACK | TCP_FLAG_PSH, 8006, tcp_conn.snd_nxt, (const u8 *)"abc", 3, 0, 1));
    CHECK(tcp_conn.recv_len == before + 8, "odd-length payload with good checksum is accepted");
}

/* ------------------------------------------------------------------ C-04 */
static u16 ip_hdr_csum(const u8 *h, int n) {
    u32 sum = 0;
    for (int i = 0; i + 1 < n; i += 2) sum += (h[i] << 8) | h[i + 1];
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)~sum;
}
static int build_ip(u8 *buf, u8 proto, u32 src, u32 dst, const u8 *payload, u16 plen, int good) {
    memset(buf, 0, 20);
    buf[0] = 0x45; buf[2] = (u8)((20 + plen) >> 8); buf[3] = (u8)(20 + plen); buf[8] = 64; buf[9] = proto;
    for (int i = 0; i < 4; i++) { buf[12 + i] = src >> (24 - 8 * i); buf[16 + i] = dst >> (24 - 8 * i); }
    u16 c = ip_hdr_csum(buf, 20); if (!good) c ^= 0x0004;
    buf[10] = c >> 8; buf[11] = c & 0xFF;
    memcpy(buf + 20, payload, plen);
    return 20 + plen;
}
static int udp_hits = 0; static u16 udp_last_len = 0;
static void udp_test_handler(u32 src, u16 sport, const u8 *d, u16 n) { (void)src; (void)sport; (void)d; udp_hits++; udp_last_len = n; }
static u16 build_udp(u8 *u, u16 sport, u16 dport, const u8 *data, u16 dlen, int mode) { /* mode 0 good csum, 1 bad csum, 2 csum field 0 */
    u16 len = 8 + dlen;
    u[0] = sport >> 8; u[1] = sport; u[2] = dport >> 8; u[3] = dport; u[4] = len >> 8; u[5] = len; u[6] = u[7] = 0;
    memcpy(u + 8, data, dlen);
    if (mode != 2) {
        u16 c = test_csum(u, len, PEER_IP, MY_IP) ; if (c == 0) c = 0xFFFF;   /* test_csum uses proto 6 -- recompute for UDP below */
        u32 sum = (PEER_IP >> 16) + (PEER_IP & 0xFFFF) + (MY_IP >> 16) + (MY_IP & 0xFFFF) + 17 + len;
        for (u16 i = 0; i + 1 < len; i += 2) sum += (u[i] << 8) | u[i + 1];
        if (len & 1) sum += u[len - 1] << 8;
        while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
        c = (u16)~sum; if (c == 0) c = 0xFFFF;
        if (mode == 1) c ^= 0x0101;
        u[6] = c >> 8; u[7] = c;
    }
    return len;
}
static void test_rx_checksums(void) {
    u8 pkt[200], payload[64]; ip_packet_t parsed;
    for (int i = 0; i < 64; i++) payload[i] = (u8)i;

    int n = build_ip(pkt, 17, PEER_IP, MY_IP, payload, 32, 1);
    CHECK(ip_parse(pkt, (u16)n, &parsed) == 1 && parsed.payload_len == 32 && parsed.src_ip == PEER_IP, "IPv4 header with a good checksum is parsed");
    n = build_ip(pkt, 17, PEER_IP, MY_IP, payload, 32, 0);
    CHECK(ip_parse(pkt, (u16)n, &parsed) == 0, "IPv4 header with a bad checksum is rejected");
    n = build_ip(pkt, 17, PEER_IP, MY_IP, payload, 32, 1); pkt[15] ^= 0x01;   /* flip a bit in the source address after checksumming */
    CHECK(ip_parse(pkt, (u16)n, &parsed) == 0, "a header altered after checksumming (spoofed source) is rejected");

    /* ethernet padding after total_len must not matter */
    n = build_ip(pkt, 17, PEER_IP, MY_IP, payload, 5, 1); memset(pkt + n, 0, 20);
    CHECK(ip_parse(pkt, (u16)(n + 20), &parsed) == 1 && parsed.payload_len == 5, "frame padding after total_len is ignored");

    /* UDP */
    udp_init(); udp_listen(5353, udp_test_handler);
    ip_packet_t ip; memset(&ip, 0, sizeof ip); ip.proto = IP_PROTO_UDP; ip.src_ip = PEER_IP; ip.dst_ip = MY_IP;
    u8 udp[120]; u8 d[11] = "hello-world";
    udp_hits = 0;
    ip.payload = udp; ip.payload_len = build_udp(udp, 53, 5353, d, 11, 0); udp_handle_packet(&ip);
    CHECK(udp_hits == 1 && udp_last_len == 11, "UDP datagram with a good checksum reaches the listener (odd length 11)");
    ip.payload_len = build_udp(udp, 53, 5353, d, 11, 1); udp_handle_packet(&ip);
    CHECK(udp_hits == 1, "UDP datagram with a bad checksum is dropped");
    ip.payload_len = build_udp(udp, 53, 5353, d, 11, 2); udp_handle_packet(&ip);
    CHECK(udp_hits == 2, "UDP checksum field 0 (\"not computed\") is still accepted");
    ip.payload_len = build_udp(udp, 53, 5353, d, 10, 0); udp[9] ^= 0x40; udp_handle_packet(&ip);
    CHECK(udp_hits == 2, "UDP payload corrupted after checksumming is dropped");
}

/* ------------------------------------------------------------------ C-05 */
static void establish(void) {
    new_conn();
    deliver(build_seg(TCP_FLAG_SYN | TCP_FLAG_ACK, 5000, tcp_conn.snd_nxt, 0, 0, 1, 1));
}
static void test_send_failures(void) {
    u8 ipbuf[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

    /* ip_send's three outcomes */
    net_setup();
    CHECK(ip_send(PEER_IP, IP_PROTO_TCP, ipbuf, 8) == IP_SEND_SENT, "ip_send: resolved neighbor => SENT");
    fake_send_ok = 0;
    CHECK(ip_send(PEER_IP, IP_PROTO_TCP, ipbuf, 8) == IP_SEND_FAILED, "ip_send: NIC refuses the frame => FAILED");
    net_setup(); arp_put(PEER_IP, 0);
    CHECK(ip_send(PEER_IP, IP_PROTO_TCP, ipbuf, 8) == IP_SEND_QUEUED && ip_pending.pending == 1, "ip_send: unresolved neighbor => QUEUED (and really parked)");
    { u8 mac[6] = {1,2,3,4,5,6}; int before = sent_frames; ip_flush_pending(PEER_IP, mac);
      CHECK(sent_frames == before + 1 && ip_pending.pending == 0, "queued packet goes out when ARP resolves"); }
    net_setup(); nic.present = 0;
    CHECK(ip_send(PEER_IP, IP_PROTO_TCP, ipbuf, 8) == IP_SEND_FAILED, "ip_send: no NIC => FAILED");
    net_setup(); net_cfg.ready = 0;
    CHECK(ip_send(PEER_IP, IP_PROTO_TCP, ipbuf, 8) == IP_SEND_FAILED, "ip_send: no IP address yet => FAILED");
    net_setup(); arp_put(PEER_IP, 0); { static u8 big[600]; CHECK(ip_send(PEER_IP, IP_PROTO_TCP, big, 600) == IP_SEND_FAILED, "ip_send: too big to park while ARP resolves => FAILED"); }

    /* connect: SYN that cannot leave => fail NOW, not after the retransmit budget */
    net_setup(); nic.present = 0; tcp_connect(PEER_IP, PEER_PORT);
    CHECK(tcp_conn.state == TCP_CLOSED, "connect with no NIC fails immediately (state %d)", (int)tcp_conn.state);
    net_setup(); fake_send_ok = 0; tcp_connect(PEER_IP, PEER_PORT);
    CHECK(tcp_conn.state == TCP_CLOSED && tcp_conn.retx_pending == 0, "connect with a refusing NIC fails immediately, nothing armed");
    net_setup(); arp_put(PEER_IP, 0); tcp_connect(PEER_IP, PEER_PORT);
    CHECK(tcp_conn.state == TCP_SYN_SENT && tcp_conn.retx_pending == 1 && ip_pending.pending == 1, "connect behind an ARP lookup stays SYN_SENT (SYN is queued, not lost)");

    /* data: refused segment must not advance snd_nxt or arm a retransmit */
    establish();
    CHECK(tcp_conn.state == TCP_ESTABLISHED, "(setup) established");
    fake_send_ok = 0; net_ticks += 1000;
    u32 nxt = tcp_conn.snd_nxt;
    CHECK(tcp_send_data((const u8 *)"hello", 5) == 1, "tcp_send_data still accepts bytes into the queue");
    CHECK(tcp_conn.snd_nxt == nxt, "refused data segment does NOT advance snd_nxt (%u -> %u)", nxt, tcp_conn.snd_nxt);
    CHECK(tcp_conn.retx_pending == 0 && tcp_conn.send_len == 5 && tcp_conn.tx_fail_count == 1, "...no retransmit armed, bytes stay queued, failure counted");
    int fr = sent_frames; net_ticks += 50; tcp_poll_retransmit();
    CHECK(tcp_conn.tx_fail_count == 1 && sent_frames == fr, "backoff: not retried before TCP_TX_FAIL_BACKOFF_TICKS");
    fake_send_ok = 1; net_ticks += TCP_TX_FAIL_BACKOFF_TICKS; tcp_poll_retransmit();
    CHECK(tcp_conn.retx_pending == 1 && tcp_conn.snd_nxt == nxt + 5 && tcp_conn.tx_fail_count == 0 && sent_frames == fr + 1,
          "after the backoff the segment goes out once, THEN snd_nxt advances and retransmit arms");

    /* NIC stays dead: connection is given up instead of hanging forever */
    establish(); fake_send_ok = 0; tcp_send_data((const u8 *)"x", 1);
    for (int k = 0; k < TCP_MAX_TX_FAILS + 2 && tcp_conn.state == TCP_ESTABLISHED; k++) { net_ticks += TCP_TX_FAIL_BACKOFF_TICKS; tcp_poll_retransmit(); }
    CHECK(tcp_conn.state == TCP_CLOSED, "a permanently refusing NIC ends in CLOSED, not an endless wait (state %d)", (int)tcp_conn.state);

    /* FIN refused: stays pending, state unchanged, goes out later */
    establish(); fake_send_ok = 0; net_ticks += 1000; tcp_close();
    CHECK(tcp_conn.state == TCP_ESTABLISHED && tcp_conn.fin_pending == 1, "refused FIN stays pending, state not advanced to FIN_WAIT_1");
    fake_send_ok = 1; net_ticks += TCP_TX_FAIL_BACKOFF_TICKS; tcp_poll_retransmit();
    CHECK(tcp_conn.state == TCP_FIN_WAIT_1 && tcp_conn.fin_pending == 0, "FIN is sent once the NIC accepts it");
}

int main(void) {
    signal(SIGALRM, on_alarm); alarm(60);
    /* the kernel's fixed network-buffer region, as ordinary memory */
    void *m = mmap((void *)MW_NETMEM_BASE, MW_NETMEM_END - MW_NETMEM_BASE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (m != (void *)(unsigned long)MW_NETMEM_BASE) { printf("cannot map 0x%X\n", MW_NETMEM_BASE); return 3; }

    test_ata_timeouts();
    test_partial_program();
    test_tcp_synack();
    test_rx_checksums();
    test_send_failures();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
