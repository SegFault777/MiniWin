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
static int      cut_after = -1;      /* >=0: after this many completed sector writes, every later write fails (the machine "lost power") */
static int      sector_writes_done;
static unsigned long io_polls;      /* every status read, to show the loops are bounded */

static u32 fake_wall = 36000;      /* RTC seconds-of-day the fake CMOS reports (tests advance it) */
static u8  cmos_reg;
static u8 inb(u16 port) {
    if (port == 0x71) {                                   /* CMOS data: a binary-mode, 24h RTC reading fake_wall */
        switch (cmos_reg) {
            case 0x00: return (u8)(fake_wall % 60);
            case 0x02: return (u8)((fake_wall / 60) % 60);
            case 0x04: return (u8)((fake_wall / 3600) % 24);
            case 0x07: return 1; case 0x08: return 1; case 0x09: return 26;
            case 0x0B: return 0x06;                       /* binary + 24-hour */
            default:   return 0;                          /* incl. 0x0A: no update in progress */
        }
    }
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
    if (cur_cmd == 0x30 && cut_after >= 0 && sector_writes_done >= cut_after) return 0x41;
    if (cur_cmd == 0x20 || cur_cmd == 0x30) return xfer_words < 256 ? 0x48 : 0x40; /* RDY|DRQ */
    return 0x40;
}
static void outb(u16 port, u8 val) {
    if (port == 0x70) { cmos_reg = val; return; }
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
    if (++xfer_words == 256) { memcpy(&disk[cur_lba * 512], wr_sector, 512); cur_cmd = 0x31; flush_done = 0; sector_writes_done++; }
}
static void outl(u16 p, u32 v) { (void)p; (void)v; }
static u32  inl(u16 p) { (void)p; return 0; }
static void io_wait(void) {}

#include "../../kernel/ata.h"
#include "../../kernel/fs.h"
#include "../../kernel/tcp.h"
#include "../../kernel/udp.h"
#include "../../kernel/dns.h"
#include "../../kernel/dhcp.h"
#include "../../kernel/tls.h"

static int checks = 0, failures = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void on_alarm(int s) { (void)s; printf("FAIL: HANG (a bounded wait never returned)\n"); _exit(2); }

static void disk_reset(void) {
    memset(disk, 0, sizeof disk);
    ata_mode = MODE_OK; ata_err_lba = 0xFFFFFFFFu; cur_cmd = 0; flush_done = 0;
    cut_after = -1; sector_writes_done = 0; fs_doc_journal_ok = 0; fs_prog_journal_ok = 0;
}
static void reboot(void) { cut_after = -1; ata_mode = MODE_OK; fs_doc_journal_ok = 0; fs_prog_journal_ok = 0; }   /* power back on: nothing is remembered in RAM */

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
    for (int k = 0; k < 60; k++) { net_ticks += TCP_TX_FAIL_BACKOFF_TICKS; tcp_poll_retransmit(); }   /* many refusals, but within the same RTC second */
    CHECK(tcp_conn.state == TCP_ESTABLISHED && tcp_conn.tx_fail_count >= 60, "refusals inside one real second do NOT drop the connection (a fast CPU racks up passes quickly)");
    for (int k = 0; k < 10 && tcp_conn.state == TCP_ESTABLISHED; k++) { fake_wall += 3; net_ticks += TCP_TX_FAIL_BACKOFF_TICKS; tcp_poll_retransmit(); }
    CHECK(tcp_conn.state == TCP_CLOSED, "a NIC that keeps refusing for %d real seconds ends in CLOSED, not an endless wait (state %d)", TCP_TX_FAIL_GIVEUP_SECS, (int)tcp_conn.state);
    /* a stopped RTC must not mean "never give up" */
    establish(); fake_send_ok = 0; tcp_send_data((const u8 *)"x", 1);
    for (int k = 0; k < TCP_TX_FAIL_BACKSTOP + 2 && tcp_conn.state == TCP_ESTABLISHED; k++) { net_ticks += TCP_TX_FAIL_BACKOFF_TICKS; tcp_poll_retransmit(); }
    CHECK(tcp_conn.state == TCP_CLOSED, "with a stopped RTC the refusal-count backstop still ends the connection");

    /* FIN refused: stays pending, state unchanged, goes out later */
    establish(); fake_send_ok = 0; net_ticks += 1000; tcp_close();
    CHECK(tcp_conn.state == TCP_ESTABLISHED && tcp_conn.fin_pending == 1, "refused FIN stays pending, state not advanced to FIN_WAIT_1");
    fake_send_ok = 1; net_ticks += TCP_TX_FAIL_BACKOFF_TICKS; tcp_poll_retransmit();
    CHECK(tcp_conn.state == TCP_FIN_WAIT_1 && tcp_conn.fin_pending == 0, "FIN is sent once the NIC accepts it");
}

/* ================================================================== rc-3 */
/* ------------------------------------------------------------------ G-09 */
static void test_rst(void) {
    /* SYN_SENT: only a RST that ACKs our SYN counts */
    new_conn(); u32 want = tcp_conn.snd_nxt;
    deliver(build_seg(TCP_FLAG_RST, 0, 0, 0, 0, 0, 1));
    CHECK(tcp_conn.state == TCP_SYN_SENT, "SYN_SENT: RST without ACK is ignored");
    deliver(build_seg(TCP_FLAG_RST | TCP_FLAG_ACK, 0, want + 5, 0, 0, 0, 1));
    CHECK(tcp_conn.state == TCP_SYN_SENT, "SYN_SENT: RST|ACK with a wrong ACK number is ignored");
    deliver(build_seg(TCP_FLAG_RST | TCP_FLAG_ACK, 0, want, 0, 0, 0, 1));
    CHECK(tcp_conn.state == TCP_CLOSED, "SYN_SENT: RST|ACK acknowledging our SYN closes the connection (connection refused)");

    /* ESTABLISHED */
    establish(); u32 rcv = tcp_conn.rcv_nxt;
    deliver(build_seg(TCP_FLAG_RST, rcv + 100000, 0, 0, 0, 0, 1));
    CHECK(tcp_conn.state == TCP_ESTABLISHED, "ESTABLISHED: RST far outside the receive window is ignored");
    deliver(build_seg(TCP_FLAG_RST, rcv - 1, 0, 0, 0, 0, 1));
    CHECK(tcp_conn.state == TCP_ESTABLISHED, "ESTABLISHED: RST with an old sequence number is ignored");
    int fr = sent_frames;
    deliver(build_seg(TCP_FLAG_RST, rcv + 10, 0, 0, 0, 0, 1));
    CHECK(tcp_conn.state == TCP_ESTABLISHED && sent_frames == fr + 1, "ESTABLISHED: RST inside the window but not exact => challenge ACK, connection kept");
    deliver(build_seg(TCP_FLAG_RST, rcv, 0, 0, 0, 0, 0));
    CHECK(tcp_conn.state == TCP_ESTABLISHED, "ESTABLISHED: RST with a bad checksum is ignored");
    deliver(build_seg(TCP_FLAG_RST, rcv, 0, 0, 0, 0, 1));
    CHECK(tcp_conn.state == TCP_CLOSED, "ESTABLISHED: RST with seq == rcv_nxt resets");
}

/* ------------------------------------------------------------------ G-04 */
static u8 arp_frame[64];
static const u8 MAC_A[6] = { 0x52, 0x54, 0x00, 0x12, 0x35, 0x02 };
static const u8 MAC_EVIL[6] = { 0x02, 0xEE, 0xEE, 0xEE, 0xEE, 0x01 };
static void build_arp(u16 op, const u8 *smac, u32 sip, const u8 *tmac, u32 tip, const u8 *eth_src) {
    memset(arp_frame, 0, sizeof arp_frame);
    memset(arp_frame, 0xFF, 6); memcpy(arp_frame + 6, eth_src, 6);
    arp_frame[12] = 0x08; arp_frame[13] = 0x06;
    arp_frame[14] = 0; arp_frame[15] = 1; arp_frame[16] = 0x08; arp_frame[17] = 0; arp_frame[18] = 6; arp_frame[19] = 4;
    arp_frame[20] = op >> 8; arp_frame[21] = (u8)op;
    memcpy(arp_frame + 22, smac, 6);
    for (int i = 0; i < 4; i++) { arp_frame[28 + i] = sip >> (24 - 8 * i); arp_frame[38 + i] = tip >> (24 - 8 * i); }
    memcpy(arp_frame + 32, tmac, 6);
}
static int arp_has(u32 ip, u8 mac_out[6]) { return arp_lookup(ip, mac_out); }
static void test_arp(void) {
    u8 got[6]; const u32 H1 = 0x0A000250u, GW = 0x0A000202u;
    net_setup(); arp_put(GW, 0);                          /* empty cache */

    /* unsolicited reply (nobody asked) must not create an entry */
    build_arp(2, MAC_EVIL, GW, nic.mac, MY_IP, MAC_EVIL); arp_handle_frame(arp_frame, 42);
    CHECK(!arp_has(GW, got), "an unsolicited ARP reply creates no cache entry");

    /* we ask; a reply naming a DIFFERENT host's address than we asked about is not accepted for the pending one */
    arp_send_request(MY_IP, GW);
    CHECK(arp_is_pending(GW), "request marks the entry pending");
    build_arp(2, MAC_EVIL, H1, nic.mac, MY_IP, MAC_EVIL); arp_handle_frame(arp_frame, 42);
    CHECK(!arp_has(GW, got) && !arp_has(H1, got), "reply for an address we did not ask about is ignored");
    /* reply addressed to someone else */
    build_arp(2, MAC_A, GW, MAC_EVIL, MY_IP, MAC_A); arp_handle_frame(arp_frame, 42);
    CHECK(!arp_has(GW, got), "reply whose target MAC is not ours is ignored");
    build_arp(2, MAC_A, GW, nic.mac, MY_IP + 1, MAC_A); arp_handle_frame(arp_frame, 42);
    CHECK(!arp_has(GW, got), "reply whose target IP is not ours is ignored");
    /* L2 source different from the ARP sender hardware address */
    build_arp(2, MAC_A, GW, nic.mac, MY_IP, MAC_EVIL); arp_handle_frame(arp_frame, 42);
    CHECK(!arp_has(GW, got), "reply whose Ethernet source != ARP sender MAC is ignored");
    /* multicast sender MAC / sender 0.0.0.0 */
    { u8 mc[6] = { 0x01, 0x00, 0x5E, 0, 0, 1 }; build_arp(2, mc, GW, nic.mac, MY_IP, mc); arp_handle_frame(arp_frame, 42); }
    CHECK(!arp_has(GW, got), "reply from a multicast MAC is ignored");
    /* the genuine, solicited reply */
    build_arp(2, MAC_A, GW, nic.mac, MY_IP, MAC_A); arp_handle_frame(arp_frame, 42);
    CHECK(arp_has(GW, got) && memcmp(got, MAC_A, 6) == 0 && !arp_is_pending(GW), "the solicited reply resolves the entry");

    /* poisoning an EXISTING entry: unsolicited reply and request both leave it alone */
    build_arp(2, MAC_EVIL, GW, nic.mac, MY_IP, MAC_EVIL); arp_handle_frame(arp_frame, 42);
    arp_has(GW, got); CHECK(memcmp(got, MAC_A, 6) == 0, "unsolicited reply cannot overwrite a resolved entry");
    build_arp(1, MAC_EVIL, GW, (u8 *)"\0\0\0\0\0\0", MY_IP, MAC_EVIL); arp_handle_frame(arp_frame, 42);
    arp_has(GW, got); CHECK(memcmp(got, MAC_A, 6) == 0, "a who-has-us REQUEST cannot overwrite a resolved entry either");

    /* a request that asks about US: we answer, and learn the asker in a free slot */
    int fr = sent_frames;
    build_arp(1, MAC_EVIL, H1, (u8 *)"\0\0\0\0\0\0", MY_IP, MAC_EVIL); arp_handle_frame(arp_frame, 42);
    CHECK(sent_frames == fr + 1, "who-has-us request is answered");
    CHECK(arp_has(H1, got) && memcmp(got, MAC_EVIL, 6) == 0, "asker is learned into a free slot");
    /* a request about somebody else teaches us nothing (the old code learned from every packet) */
    fr = sent_frames;
    build_arp(1, MAC_EVIL, 0x0A000299u, (u8 *)"\0\0\0\0\0\0", 0x0A000298u, MAC_EVIL); arp_handle_frame(arp_frame, 42);
    CHECK(!arp_has(0x0A000299u, got) && sent_frames == fr, "a request for another host is not learned from and not answered");
    /* someone claiming OUR address */
    build_arp(2, MAC_EVIL, MY_IP, nic.mac, MY_IP, MAC_EVIL); arp_handle_frame(arp_frame, 42);
    CHECK(!arp_has(MY_IP, got), "a packet claiming our own IP is ignored");

    /* a lost request must not block resolution forever */
    net_setup(); arp_put(GW, 0); arp_send_request(MY_IP, GW);
    CHECK(arp_is_pending(GW), "pending right after the request");
    net_ticks += ARP_PENDING_TIMEOUT_TICKS + 5;
    CHECK(!arp_is_pending(GW), "pending expires after ARP_PENDING_TIMEOUT_TICKS (so ip_send asks again)");
    fr = sent_frames; { u8 b[8] = {0}; ip_send(GW, IP_PROTO_TCP, b, 8); }
    CHECK(sent_frames == fr + 1 && arp_is_pending(GW), "ip_send re-issues the who-has once the old one expired");
    /* a LATE genuine reply is still honored */
    net_ticks += ARP_PENDING_TIMEOUT_TICKS + 5;
    build_arp(2, MAC_A, GW, nic.mac, MY_IP, MAC_A); arp_handle_frame(arp_frame, 42);
    CHECK(arp_has(GW, got), "a late reply to our (expired) request still resolves it");
}

/* ------------------------------------------------------------------ G-05 */
static u8 dnsp[512]; static u16 dnsl;
static void dns_begin(u16 id, u16 flags, const char *qname, u16 qtype, u16 qclass, int qd, int an) {
    memset(dnsp, 0, sizeof dnsp);
    dnsp[0] = id >> 8; dnsp[1] = (u8)id; dnsp[2] = flags >> 8; dnsp[3] = (u8)flags;
    dnsp[4] = 0; dnsp[5] = (u8)qd; dnsp[6] = 0; dnsp[7] = (u8)an;
    dnsl = 12;
    if (qd) {
        const char *p = qname;
        while (*p) { const char *d = p; while (*d && *d != '.') d++; dnsp[dnsl++] = (u8)(d - p); memcpy(dnsp + dnsl, p, d - p); dnsl += (u16)(d - p); p = *d ? d + 1 : d; }
        dnsp[dnsl++] = 0;
        dnsp[dnsl++] = qtype >> 8; dnsp[dnsl++] = (u8)qtype; dnsp[dnsl++] = qclass >> 8; dnsp[dnsl++] = (u8)qclass;
    }
}
static void dns_rr(const u8 *owner, u16 olen, u16 type, const u8 *rdata, u16 rlen) {
    memcpy(dnsp + dnsl, owner, olen); dnsl += olen;
    dnsp[dnsl++] = type >> 8; dnsp[dnsl++] = (u8)type; dnsp[dnsl++] = 0; dnsp[dnsl++] = 1;
    dnsp[dnsl++] = 0; dnsp[dnsl++] = 0; dnsp[dnsl++] = 1; dnsp[dnsl++] = 0x2C;
    dnsp[dnsl++] = rlen >> 8; dnsp[dnsl++] = (u8)rlen; memcpy(dnsp + dnsl, rdata, rlen); dnsl += rlen;
}
static void dns_new(void) { net_setup(); net_cfg.dns_ip = 0x0A000203u; arp_put(0x0A000203u, 1); dns_resolve("www.example.com"); }
static void test_dns(void) {
    const u32 SRV = 0x0A000203u; const u8 ptr_q[2] = { 0xC0, 0x0C }; const u8 ip1[4] = { 93, 184, 216, 34 };
    u16 id;

    dns_new(); id = dns_client.query_id;
    dns_begin(id, 0x8180, "www.example.com", 1, 1, 1, 1); dns_rr(ptr_q, 2, 1, ip1, 4);
    dns_handle_reply(SRV, 53, dnsp, dnsl);
    CHECK(dns_client.state == DNS_RESOLVED && dns_client.result_ip == 0x5DB8D822u, "a well-formed answer resolves (ip %08X)", dns_client.result_ip);

    dns_new(); id = dns_client.query_id;
    dns_begin(id, 0x8180, "WWW.Example.COM", 1, 1, 1, 1); dns_rr(ptr_q, 2, 1, ip1, 4); dns_handle_reply(SRV, 53, dnsp, dnsl);
    CHECK(dns_client.state == DNS_RESOLVED, "name comparison is case-insensitive");

    dns_new(); id = dns_client.query_id;
    dns_begin(id, 0x8180, "www.example.com", 1, 1, 1, 1); dns_rr(ptr_q, 2, 1, ip1, 4);
    dns_handle_reply(SRV, 5353, dnsp, dnsl);
    CHECK(dns_client.state == DNS_QUERYING, "answer from source port != 53 is ignored");
    dns_begin(id, 0x0180, "www.example.com", 1, 1, 1, 1); dns_rr(ptr_q, 2, 1, ip1, 4); dns_handle_reply(SRV, 53, dnsp, dnsl);
    CHECK(dns_client.state == DNS_QUERYING, "a packet with QR=0 (a query, not a response) is ignored");
    dns_begin(id, 0x8180, "evil.example.org", 1, 1, 1, 1); { u8 ev[2] = {0xC0, 0x0C}; dns_rr(ev, 2, 1, ip1, 4); } dns_handle_reply(SRV, 53, dnsp, dnsl);
    CHECK(dns_client.state == DNS_QUERYING, "right ID but the echoed question is a different name => ignored");
    dns_begin(id, 0x8180, "www.example.com", 28, 1, 1, 1); dns_rr(ptr_q, 2, 1, ip1, 4); dns_handle_reply(SRV, 53, dnsp, dnsl);
    CHECK(dns_client.state == DNS_QUERYING, "echoed QTYPE != A => ignored");
    dns_begin(id, 0x8180, "www.example.com", 1, 3, 1, 1); dns_rr(ptr_q, 2, 1, ip1, 4); dns_handle_reply(SRV, 53, dnsp, dnsl);
    CHECK(dns_client.state == DNS_QUERYING, "echoed QCLASS != IN => ignored");
    dns_begin(id, 0x8180, "", 1, 1, 0, 1); { u8 o[2] = {0xC0, 0x0C}; dns_rr(o, 2, 1, ip1, 4); } dns_handle_reply(SRV, 53, dnsp, dnsl);
    CHECK(dns_client.state == DNS_QUERYING, "NOERROR answer with no echoed question => ignored");
    dns_begin(id, 0x8183, "", 1, 1, 0, 0); dns_handle_reply(SRV, 53, dnsp, dnsl);
    CHECK(dns_client.state == DNS_QUERYING, "bare NXDOMAIN with no echoed question => ignored (cannot be used to deny service)");
    dns_begin(id, 0x8183, "www.example.com", 1, 1, 1, 0); dns_handle_reply(SRV, 53, dnsp, dnsl);
    CHECK(dns_client.state == DNS_FAILED, "NXDOMAIN that echoes our question still reports 'no such host'");

    /* answers for a different owner name */
    dns_new(); id = dns_client.query_id;
    { u8 other[17] = { 3,'e','v','l', 3,'c','o','m', 0 }; dns_begin(id, 0x8180, "www.example.com", 1, 1, 1, 1); dns_rr(other, 9, 1, ip1, 4); }
    dns_handle_reply(SRV, 53, dnsp, dnsl);
    CHECK(dns_client.state == DNS_QUERYING && dns_client.result_ip == 0, "an A record owned by another name is not accepted (and does not end the lookup)");
    { u8 other[9] = { 3,'e','v','l', 3,'c','o','m', 0 }; dns_begin(id, 0x8180, "www.example.com", 1, 1, 1, 2);
      const u8 evil[4] = { 6, 6, 6, 6 }; dns_rr(other, 9, 1, evil, 4); dns_rr(ptr_q, 2, 1, ip1, 4); }
    dns_handle_reply(SRV, 53, dnsp, dnsl);
    CHECK(dns_client.state == DNS_RESOLVED && dns_client.result_ip == 0x5DB8D822u, "a foreign-owner record is skipped, the correctly-owned one is used");

    /* CNAME chain: www.example.com -> cdn.example.net -> A */
    dns_new(); id = dns_client.query_id;
    { u8 target[] = { 3,'c','d','n', 7,'e','x','a','m','p','l','e', 3,'n','e','t', 0 };
      dns_begin(id, 0x8180, "www.example.com", 1, 1, 1, 2); dns_rr(ptr_q, 2, 5, target, sizeof target);
      u16 tgt_off = (u16)(dnsl - sizeof target);               /* where the CNAME target sits in the packet */
      u8 own2[2] = { (u8)(0xC0 | (tgt_off >> 8)), (u8)tgt_off }; dns_rr(own2, 2, 1, ip1, 4); }
    dns_handle_reply(SRV, 53, dnsp, dnsl);
    CHECK(dns_client.state == DNS_RESOLVED && dns_client.result_ip == 0x5DB8D822u, "CNAME chain followed: A record owned by the CNAME target is accepted");
    /* A record for the CNAME target's name WITHOUT the CNAME that links it to our question: foreign */
    dns_new(); id = dns_client.query_id;
    { u8 target[] = { 3,'c','d','n', 3,'n','e','t', 0 };
      dns_begin(id, 0x8180, "www.example.com", 1, 1, 1, 1); dns_rr(target, sizeof target, 1, ip1, 4); }
    dns_handle_reply(SRV, 53, dnsp, dnsl);
    CHECK(dns_client.state == DNS_QUERYING, "an A record for an unrelated name (no CNAME linking it) is rejected");

    /* malformed compression: a forward/self-referencing pointer in the owner name */
    dns_new(); id = dns_client.query_id;
    dns_begin(id, 0x8180, "www.example.com", 1, 1, 1, 1); { u16 self = dnsl; u8 loop[2] = { (u8)(0xC0 | (self >> 8)), (u8)self }; dns_rr(loop, 2, 1, ip1, 4); }
    dns_handle_reply(SRV, 53, dnsp, dnsl);
    CHECK(dns_client.state == DNS_QUERYING && dns_client.result_ip == 0, "a self-referencing compression pointer is rejected, not followed");
}

/* ------------------------------------------------------------------ G-10 / G-11 */
static u8 dhp[400];
static u16 build_dhcp(u8 op, u32 xid, const u8 *chaddr, u32 yiaddr, int msgtype, int sid, u32 server, u32 lease, int with_lease) {
    memset(dhp, 0, sizeof dhp);
    dhp[0] = op; dhp[1] = 1; dhp[2] = 6;
    for (int i = 0; i < 4; i++) { dhp[4 + i] = xid >> (24 - 8 * i); dhp[16 + i] = yiaddr >> (24 - 8 * i); }
    memcpy(dhp + 28, chaddr, 6);
    u16 pos = 236; dhp[pos++] = 0x63; dhp[pos++] = 0x82; dhp[pos++] = 0x53; dhp[pos++] = 0x63;
    dhp[pos++] = 53; dhp[pos++] = 1; dhp[pos++] = (u8)msgtype;
    if (sid) { dhp[pos++] = 54; dhp[pos++] = 4; for (int i = 0; i < 4; i++) dhp[pos++] = server >> (24 - 8 * i); }
    dhp[pos++] = 1; dhp[pos++] = 4; dhp[pos++] = 255; dhp[pos++] = 255; dhp[pos++] = 255; dhp[pos++] = 0;
    dhp[pos++] = 3; dhp[pos++] = 4; dhp[pos++] = 10; dhp[pos++] = 0; dhp[pos++] = 2; dhp[pos++] = 2;
    dhp[pos++] = 6; dhp[pos++] = 4; dhp[pos++] = 10; dhp[pos++] = 0; dhp[pos++] = 2; dhp[pos++] = 3;
    if (with_lease) { dhp[pos++] = 51; dhp[pos++] = 4; for (int i = 0; i < 4; i++) dhp[pos++] = lease >> (24 - 8 * i); }
    dhp[pos++] = 255;
    return pos;
}
static void dhcp_poll_after(u32 secs) { fake_wall += secs; net_ticks += 100; dhcp_poll(); }
static void test_dhcp(void) {
    const u32 SRVIP = 0x0A000202u, YI = 0x0A00020Fu;
    net_setup(); net_cfg.ready = 0; net_cfg.my_ip = 0; net_cfg.netmask = 0; net_cfg.gateway_ip = 0; net_cfg.dns_ip = 0;
    dhcp_state = DHCP_STATE_IDLE; dhcp_started = 0; dhcp_lease_secs = 0;
    udp_init(); dhcp_start();
    CHECK(dhcp_state == DHCP_STATE_DISCOVER_SENT, "dhcp_start sends DISCOVER");
    CHECK(dhcp_xid != 0x1234ABCDu, "the first conversation no longer uses the old constant xid");
    u32 xids[64]; int distinct = 1;
    for (int i = 0; i < 64; i++) { net_ticks += 7; xids[i] = dhcp_new_xid(); dhcp_xid = xids[i]; for (int k = 0; k < i; k++) if (xids[k] == xids[i]) distinct = 0; }
    CHECK(distinct, "64 consecutive transaction ids are all different");
    dhcp_xid = dhcp_new_xid(); u32 xid = dhcp_xid;
    u16 n;

    n = build_dhcp(2, xid, nic.mac, YI, 2, 1, SRVIP, 3600, 1);
    dhcp_handle_reply(SRVIP, 68, dhp, n); CHECK(dhcp_state == DHCP_STATE_DISCOVER_SENT, "OFFER from source port 68 (not 67) is ignored");
    n = build_dhcp(1, xid, nic.mac, YI, 2, 1, SRVIP, 3600, 1);
    dhcp_handle_reply(SRVIP, 67, dhp, n); CHECK(dhcp_state == DHCP_STATE_DISCOVER_SENT, "a BOOTREQUEST (op=1) is not accepted as a reply");
    n = build_dhcp(2, xid + 1, nic.mac, YI, 2, 1, SRVIP, 3600, 1);
    dhcp_handle_reply(SRVIP, 67, dhp, n); CHECK(dhcp_state == DHCP_STATE_DISCOVER_SENT, "wrong xid is ignored");
    n = build_dhcp(2, xid, MAC_EVIL, YI, 2, 1, SRVIP, 3600, 1);
    dhcp_handle_reply(SRVIP, 67, dhp, n); CHECK(dhcp_state == DHCP_STATE_DISCOVER_SENT, "a reply for another client's MAC (chaddr) is ignored");
    n = build_dhcp(2, xid, nic.mac, 0, 2, 1, SRVIP, 3600, 1);
    dhcp_handle_reply(SRVIP, 67, dhp, n); CHECK(dhcp_state == DHCP_STATE_DISCOVER_SENT, "an OFFER of 0.0.0.0 is ignored");
    n = build_dhcp(2, xid, nic.mac, YI, 2, 1, SRVIP, 3600, 1);
    dhcp_handle_reply(SRVIP, 67, dhp, n);
    CHECK(dhcp_state == DHCP_STATE_REQUEST_SENT && dhcp_offered_ip == YI && dhcp_server_id == SRVIP, "a valid OFFER is accepted and answered with a REQUEST");

    /* ACK validation */
    n = build_dhcp(2, xid, nic.mac, YI, 5, 1, 0x0A0002FEu, 3600, 1);
    dhcp_handle_reply(0x0A0002FEu, 67, dhp, n); CHECK(dhcp_state == DHCP_STATE_REQUEST_SENT && !net_cfg.ready, "ACK from a different server than the one we asked is ignored");
    n = build_dhcp(2, xid, nic.mac, YI + 1, 5, 1, SRVIP, 3600, 1);
    dhcp_handle_reply(SRVIP, 67, dhp, n); CHECK(dhcp_state == DHCP_STATE_REQUEST_SENT && !net_cfg.ready, "ACK granting a different address than we requested is ignored");
    n = build_dhcp(2, xid, nic.mac, YI, 6, 1, 0x0A0002FEu, 0, 0);
    dhcp_handle_reply(0x0A0002FEu, 67, dhp, n); CHECK(dhcp_state == DHCP_STATE_REQUEST_SENT, "a NAK from a server we are not talking to is ignored");
    n = build_dhcp(2, xid, nic.mac, YI, 5, 1, SRVIP, 1000, 1);
    dhcp_handle_reply(SRVIP, 67, dhp, n);
    CHECK(dhcp_state == DHCP_STATE_BOUND && net_cfg.ready && net_cfg.my_ip == YI && net_cfg.gateway_ip == 0x0A000202u && net_cfg.dns_ip == 0x0A000203u, "a valid ACK binds");
    CHECK(dhcp_lease_secs == 1000, "the lease time (option 51) is kept (got %u)", dhcp_lease_secs);

    /* a NAK while BOUND must not knock us off the network */
    n = build_dhcp(2, dhcp_xid, nic.mac, 0, 6, 1, SRVIP, 0, 0);
    dhcp_handle_reply(SRVIP, 67, dhp, n); CHECK(dhcp_state == DHCP_STATE_BOUND && net_cfg.ready, "a NAK while BOUND is ignored");

    /* lease lifecycle: T1 = 500, T2 = 875, expiry = 1000 */
    dhcp_poll_after(300); CHECK(dhcp_state == DHCP_STATE_BOUND, "before T1: still BOUND");
    dhcp_poll_after(250);
    CHECK(dhcp_state == DHCP_STATE_RENEWING, "at T1 (half the lease) the client starts RENEWING");
    u32 renew_xid = dhcp_xid; CHECK(renew_xid != xid, "a renewal uses a fresh xid");
    n = build_dhcp(2, renew_xid, nic.mac, YI, 5, 1, SRVIP, 1000, 1);
    dhcp_handle_reply(SRVIP, 67, dhp, n);
    CHECK(dhcp_state == DHCP_STATE_BOUND && dhcp_lease_age == 0 && net_cfg.my_ip == YI, "an ACK to the renewal extends the lease (age reset)");
    dhcp_poll_after(520); CHECK(dhcp_state == DHCP_STATE_RENEWING, "renewing again at the next T1");
    dhcp_poll_after(360); CHECK(dhcp_state == DHCP_STATE_REBINDING, "at T2 (7/8 of the lease) the client starts REBINDING");
    CHECK(net_cfg.ready, "...still holding the address while it rebinds");
    dhcp_poll_after(130);
    CHECK(dhcp_state == DHCP_STATE_IDLE && !net_cfg.ready && net_cfg.my_ip == 0 && net_cfg.gateway_ip == 0 && net_cfg.dns_ip == 0,
          "when the lease runs out the address, gateway and DNS are dropped (state %d)", (int)dhcp_state);
    { u8 b[8] = {0}; CHECK(ip_send(PEER_IP, IP_PROTO_TCP, b, 8) == IP_SEND_FAILED, "...so nothing is sent from the expired address"); }

    /* renewal refused */
    net_setup(); net_cfg.ready = 0; dhcp_state = DHCP_STATE_IDLE; dhcp_started = 1;
    dhcp_xid = dhcp_new_xid(); dhcp_send_discover(); xid = dhcp_xid;
    n = build_dhcp(2, xid, nic.mac, YI, 2, 1, SRVIP, 200, 1); dhcp_handle_reply(SRVIP, 67, dhp, n);
    n = build_dhcp(2, xid, nic.mac, YI, 5, 1, SRVIP, 200, 1); dhcp_handle_reply(SRVIP, 67, dhp, n);
    CHECK(dhcp_state == DHCP_STATE_BOUND, "(setup) bound with a 200s lease");
    dhcp_poll_after(110); CHECK(dhcp_state == DHCP_STATE_RENEWING, "(setup) renewing");
    n = build_dhcp(2, dhcp_xid, nic.mac, 0, 6, 1, SRVIP, 0, 0); dhcp_handle_reply(SRVIP, 67, dhp, n);
    CHECK(dhcp_state == DHCP_STATE_IDLE && !net_cfg.ready, "a NAK to a renewal drops the lease");

    /* lease clock across midnight, and an infinite lease */
    net_setup(); net_cfg.ready = 0; dhcp_state = DHCP_STATE_IDLE; dhcp_started = 1;
    fake_wall = 86350; dhcp_xid = dhcp_new_xid(); dhcp_send_discover(); xid = dhcp_xid;
    n = build_dhcp(2, xid, nic.mac, YI, 2, 1, SRVIP, 100, 1); dhcp_handle_reply(SRVIP, 67, dhp, n);
    n = build_dhcp(2, xid, nic.mac, YI, 5, 1, SRVIP, 100, 1); dhcp_handle_reply(SRVIP, 67, dhp, n);
    dhcp_poll_after(30); CHECK(dhcp_state == DHCP_STATE_BOUND, "(midnight) 30s into a 100s lease");
    dhcp_poll_after(30); CHECK(net_wall_seconds() < 3600 && dhcp_state == DHCP_STATE_RENEWING, "(midnight) T1 reached although the RTC reading wrapped past 24:00:00 (now %u s into the day)", net_wall_seconds());
    net_setup(); net_cfg.ready = 0; dhcp_state = DHCP_STATE_IDLE; dhcp_started = 1;
    dhcp_xid = dhcp_new_xid(); dhcp_send_discover(); xid = dhcp_xid;
    n = build_dhcp(2, xid, nic.mac, YI, 2, 1, SRVIP, 0xFFFFFFFFu, 1); dhcp_handle_reply(SRVIP, 67, dhp, n);
    n = build_dhcp(2, xid, nic.mac, YI, 5, 1, SRVIP, 0xFFFFFFFFu, 1); dhcp_handle_reply(SRVIP, 67, dhp, n);
    dhcp_poll_after(5000); dhcp_poll_after(5000);
    CHECK(dhcp_state == DHCP_STATE_BOUND && net_cfg.ready, "an infinite lease never expires");
}

/* ------------------------------------------------------------------ G-06 / G-08 */
static u8 shb[300];
static u16 build_sh(u16 version, u8 sid_len, u16 suite, u8 comp, int ext_mode) {   /* ext_mode: 0 none, 1 valid reneg-info, 2 total-length mismatch, 3 truncated entry */
    memset(shb, 0, sizeof shb);
    u16 p = 0; shb[p++] = version >> 8; shb[p++] = (u8)version;
    for (int i = 0; i < 32; i++) shb[p++] = (u8)(0x40 + i);
    shb[p++] = sid_len; for (int i = 0; i < sid_len; i++) shb[p++] = (u8)i;
    shb[p++] = suite >> 8; shb[p++] = (u8)suite; shb[p++] = comp;
    if (ext_mode == 1) { shb[p++] = 0; shb[p++] = 5; shb[p++] = 0xFF; shb[p++] = 0x01; shb[p++] = 0; shb[p++] = 1; shb[p++] = 0; }
    if (ext_mode == 2) { shb[p++] = 0; shb[p++] = 9; shb[p++] = 0xFF; shb[p++] = 0x01; shb[p++] = 0; shb[p++] = 1; shb[p++] = 0; }
    if (ext_mode == 3) { shb[p++] = 0; shb[p++] = 6; shb[p++] = 0xFF; shb[p++] = 0x01; shb[p++] = 0; shb[p++] = 9; shb[p++] = 0; shb[p++] = 0; }
    return p;
}
static void tls_reset(void) { memset(&tls_conn, 0, sizeof tls_conn); tls_conn.state = TLS_CLIENT_HELLO_SENT; }
static void feed_header(u8 type, u8 vmaj, u8 vmin, u16 rec_len) {
    tls_reset();
    TLS_RX_RAW[0] = type; TLS_RX_RAW[1] = vmaj; TLS_RX_RAW[2] = vmin; TLS_RX_RAW[3] = rec_len >> 8; TLS_RX_RAW[4] = (u8)rec_len;
    tls_conn.rx_raw_len = 5;
    tls_process_raw_buffer(0);
}
static void test_tls(void) {
    /* record headers (G-08): judged as soon as the 5 header bytes are in */
    feed_header(22, 3, 3, 0xFFFF);
    CHECK(tls_conn.state == TLS_FAILED && tls_conn.fail_reason == TLS_FAIL_BAD_RECORD, "a record declaring 65535 bytes fails at once (it used to wait for bytes that can never fit)");
    feed_header(23, 3, 3, TLS_MAX_RECORD_CIPHERTEXT + 1);
    CHECK(tls_conn.state == TLS_FAILED && tls_conn.fail_reason == TLS_FAIL_BAD_RECORD, "a record one byte over the RFC 5246 ciphertext limit fails");
    feed_header(23, 3, 3, TLS_MAX_RECORD_CIPHERTEXT);
    CHECK(tls_conn.state == TLS_CLIENT_HELLO_SENT, "a record of exactly the maximum legal size is waited for, not rejected");
    feed_header(99, 3, 3, 100);
    CHECK(tls_conn.state == TLS_FAILED && tls_conn.fail_reason == TLS_FAIL_BAD_RECORD, "unknown content type fails");
    feed_header(19, 3, 3, 100);
    CHECK(tls_conn.state == TLS_FAILED, "content type 19 (below change_cipher_spec) fails");
    feed_header(24, 3, 3, 100);
    CHECK(tls_conn.state == TLS_FAILED, "content type 24 (above application_data) fails");
    feed_header(22, 2, 0, 100);
    CHECK(tls_conn.state == TLS_FAILED && tls_conn.fail_reason == TLS_FAIL_BAD_RECORD, "record version 2.0 fails");
    feed_header(22, 3, 4, 100);
    CHECK(tls_conn.state == TLS_FAILED, "record version 3.4 fails");
    feed_header(22, 3, 1, 100);
    CHECK(tls_conn.state == TLS_CLIENT_HELLO_SENT, "record version 3.1 (legal legacy value on the first records) is accepted");
    feed_header(22, 3, 3, 100);
    CHECK(tls_conn.state == TLS_CLIENT_HELLO_SENT && tls_conn.rx_raw_len == 5, "a plausible header with an incomplete body just waits");

    /* ServerHello (G-06) */
    u16 n; const u16 GOOD = TLS_SUITE_ECDHE_RSA_AES128_GCM_SHA256;
    tls_reset(); n = build_sh(0x0303, 0, GOOD, 0, 0);
    CHECK(tls_parse_server_hello(shb, n) == 1 && tls_conn.cipher_suite == GOOD, "a plain TLS 1.2 ServerHello with no extensions parses");
    tls_reset(); n = build_sh(0x0303, 32, GOOD, 0, 1);
    CHECK(tls_parse_server_hello(shb, n) == 1, "32-byte session id + a well-formed renegotiation_info extension parses");
    const u16 bad_versions[] = { 0x0302, 0x0301, 0x0300, 0x0304, 0x0200, 0xFEFF };
    for (unsigned k = 0; k < sizeof bad_versions / sizeof bad_versions[0]; k++) {
        tls_reset(); n = build_sh(bad_versions[k], 0, GOOD, 0, 0);
        CHECK(tls_parse_server_hello(shb, n) == 0 && tls_conn.fail_reason == TLS_FAIL_PROTOCOL_VERSION, "ServerHello version 0x%04X is rejected", bad_versions[k]);
    }
    tls_reset(); n = build_sh(0x0303, 0, 0x1301, 0, 0);
    CHECK(tls_parse_server_hello(shb, n) == 0 && tls_conn.fail_reason == TLS_FAIL_UNSUPPORTED_CIPHER_SUITE, "a suite we did not offer (a TLS 1.3 suite) is rejected");
    tls_reset(); n = build_sh(0x0303, 0, 0x002F, 0, 0);
    CHECK(tls_parse_server_hello(shb, n) == 0, "TLS_RSA_WITH_AES_128_CBC_SHA (never offered) is rejected");
    tls_reset(); n = build_sh(0x0303, 33, GOOD, 0, 0);
    CHECK(tls_parse_server_hello(shb, n) == 0, "session id longer than 32 bytes is rejected");
    tls_reset(); n = build_sh(0x0303, 0, GOOD, 1, 0);
    CHECK(tls_parse_server_hello(shb, n) == 0, "a non-null compression method is rejected");
    tls_reset(); n = build_sh(0x0303, 0, GOOD, 0, 2);
    CHECK(tls_parse_server_hello(shb, n) == 0, "an extensions block whose declared length does not match the message is rejected");
    tls_reset(); n = build_sh(0x0303, 0, GOOD, 0, 3);
    CHECK(tls_parse_server_hello(shb, n) == 0, "an extension whose body runs past the end is rejected");
    tls_reset(); n = build_sh(0x0303, 0, GOOD, 0, 0);
    CHECK(tls_parse_server_hello(shb, (u32)(n - 1)) == 0, "a ServerHello cut off before the compression byte is rejected");
    tls_reset();
    CHECK(tls_parse_server_hello(shb, 10) == 0, "a ServerHello shorter than its fixed part is rejected");
}

/* ------------------------------------------------------------------ G-01 / G-02 */
static u32 ref_crc32(const u8 *d, u32 n) {   /* independent reference CRC-32 */
    u32 c = 0xFFFFFFFFu;
    for (u32 i = 0; i < n; i++) { c ^= d[i]; for (int b = 0; b < 8; b++) c = (c >> 1) ^ (0xEDB88320u & (u32)-(int)(c & 1)); }
    return ~c;
}
static void fill_pat(u8 *b, u32 n, u32 seed) { for (u32 i = 0; i < n; i++) b[i] = (u8)(seed + i * 31 + (i >> 8)); }
static u8 snap_disk[sizeof disk];

static void test_fs_atomic(void) {
    static u8 A[4096], B[4096], got[4096]; u32 outlen;
    fill_pat(A, 4096, 1); fill_pat(B, 3000, 77);

    /* roundtrip + commit record */
    disk_reset();
    CHECK(fs_save_slot(1, (const char *)A, 4096) == 1, "save a full-size (4096 B) document");
    CHECK(fs_read_slot(1, (char *)got, 4096, &outlen) == 1 && outlen == 4096 && memcmp(got, A, 4096) == 0, "it reads back identically");
    { u8 *h = &disk[fs_slot_header_lba(1) * 512];
      CHECK(*(u32 *)(h + FS_HDR_COMMIT_OFF) == FS_COMMIT_MAGIC && *(u32 *)(h + FS_HDR_CRC_OFF) == ref_crc32(A, 4096), "header carries the commit marker and the correct CRC-32");
      CHECK(*(u32 *)(h + FS_HDR_TARGET_OFF) == 0, "the live header carries no journal target"); }
    { u8 *j = &disk[FS_JOURNAL_LBA * 512]; int z = 1; for (int i = 0; i < 512; i++) if (j[i]) z = 0; CHECK(z, "the journal is retired (zeroed) after a clean save"); }
    CHECK(fs_load_slot(1, (char *)got, 4096) == 4096, "the older fs_load_slot() API still works");

    /* a document that is too big is refused BEFORE anything is touched */
    memcpy(snap_disk, disk, sizeof disk);
    static u8 big[4097]; fill_pat(big, 4097, 5);
    CHECK(fs_save_slot(1, (const char *)big, 4097) == 0, "a 4097-byte document is refused (it used to be cut to 4096 and reported as saved)");
    CHECK(memcmp(snap_disk, disk, sizeof disk) == 0, "...and the disk is byte-for-byte unchanged (old file intact, journal untouched)");
    CHECK(fs_read_slot(1, (char *)got, 4096, &outlen) == 1 && memcmp(got, A, 4096) == 0, "the previous file still reads back");
    CHECK(fs_read_slot(1, (char *)got, 4000, &outlen) == 0, "reading into a buffer smaller than the file is refused, not truncated");

    /* empty document is valid and distinguishable from a damaged one */
    disk_reset();
    CHECK(fs_save_slot(0, "", 0) == 1 && fs_read_slot(0, (char *)got, 10, &outlen) == 1 && outlen == 0, "an empty document is valid (len 0)");
    CHECK(fs_read_slot(2, (char *)got, 10, &outlen) == 0, "an empty SLOT is not a document");

    /* power cut at EVERY possible point of a save over an existing file */
    int old_survived = 0, new_survived = 0, bad = 0;
    for (int k = 0; k <= 25; k++) {
        disk_reset(); fs_save_slot(2, (const char *)A, 4096); memcpy(snap_disk, disk, sizeof disk);
        sector_writes_done = 0; cut_after = k; int ok = fs_save_slot(2, (const char *)B, 3000);
        reboot();
        int rd = fs_read_slot(2, (char *)got, 4096, &outlen);
        int is_old = rd && outlen == 4096 && memcmp(got, A, 4096) == 0;
        int is_new = rd && outlen == 3000 && memcmp(got, B, 3000) == 0;
        if (!(is_old || is_new)) { bad++; printf("  power cut after %d writes => neither old nor new (rd=%d len=%u)\n", k, rd, outlen); }
        if (k < 10)  CHECK(is_old && !ok, "cut after %d sector writes (before the commit point): the OLD file survives and the save reports failure", k);
        if (k >= 10) CHECK(is_new, "cut after %d sector writes (after the commit point): the NEW file survives (completed by recovery)", k);
        if (k >= 21) CHECK(ok == 1, "with %d writes available the save succeeds", k);
        old_survived += is_old; new_survived += is_new;
    }
    CHECK(bad == 0, "no power-cut point produced a corrupt or mixed file (%d old-survives, %d new-survives, %d bad)", old_survived, new_survived, bad);

    /* the same for a first save into an EMPTY slot: before the commit point nothing exists, after it the file does */
    for (int k = 0; k <= 22; k += 1) {
        disk_reset(); sector_writes_done = 0; cut_after = k; fs_save_slot(3, (const char *)B, 3000); reboot();
        int rd = fs_read_slot(3, (char *)got, 4096, &outlen);
        if (k < 10) CHECK(!rd, "first save, cut after %d writes: the slot is still empty (not a half-written file)", k);
        else        CHECK(rd && outlen == 3000 && memcmp(got, B, 3000) == 0, "first save, cut after %d writes: the complete file exists", k);
    }

    /* corruption is detected on read */
    disk_reset(); fs_save_slot(0, (const char *)A, 4096);
    disk[fs_slot_data_lba(0) * 512 + 1500] ^= 0x10;
    CHECK(fs_read_slot(0, (char *)got, 4096, &outlen) == 0 && fs_load_slot(0, (char *)got, 4096) == 0, "a flipped bit in the data is caught by the CRC (the read fails)");
    disk_reset(); fs_save_slot(0, (const char *)A, 4096);
    memset(&disk[(fs_slot_data_lba(0) + 5) * 512], 0, 512);
    CHECK(fs_read_slot(0, (char *)got, 4096, &outlen) == 0, "a data sector that was zeroed (lost write) is caught too");
    /* legacy header (written before commit records existed): accepted, unverified */
    disk_reset(); fs_save_slot(0, (const char *)A, 4096);
    memset(&disk[fs_slot_header_lba(0) * 512 + FS_HDR_CRC_OFF], 0, 8);
    CHECK(fs_read_slot(0, (char *)got, 4096, &outlen) == 1 && memcmp(got, A, 4096) == 0, "a legacy header without a commit record is still readable");
    /* impossible length in a header */
    disk_reset(); fs_save_slot(0, (const char *)A, 4096);
    *(u32 *)&disk[fs_slot_header_lba(0) * 512 + 4] = 4097;
    { u32 l; CHECK(fs_check_slot(0, &l) == 0, "a header claiming more than FS_MAX_FILE_BYTES is not a valid slot"); }

    /* a committed-but-unapplied journal is completed before a delete, so it cannot resurrect the file */
    disk_reset(); sector_writes_done = 0; cut_after = 11; fs_save_slot(1, (const char *)B, 3000); reboot();
    CHECK(fs_delete_slot(1) == 1, "delete");
    { u32 l; CHECK(fs_check_slot(1, &l) == 0, "a deleted file stays deleted (a pending journal did not bring it back)"); }
    /* a pending journal for slot 2 is completed before a save to slot 1 starts, and neither is lost */
    disk_reset(); sector_writes_done = 0; cut_after = 12; fs_save_slot(2, (const char *)B, 3000); reboot();
    CHECK(fs_save_slot(1, (const char *)A, 4096) == 1, "a save to another slot first settles the pending journal");
    CHECK(fs_read_slot(2, (char *)got, 4096, &outlen) == 1 && outlen == 3000 && memcmp(got, B, 3000) == 0, "...the interrupted save (slot 2) is complete");
    CHECK(fs_read_slot(1, (char *)got, 4096, &outlen) == 1 && memcmp(got, A, 4096) == 0, "...and the new save (slot 1) is complete");
    /* journal whose staged data was damaged is discarded, the live file untouched */
    disk_reset(); fs_save_slot(2, (const char *)A, 4096);
    sector_writes_done = 0; cut_after = 10; fs_save_slot(2, (const char *)B, 3000); reboot();   /* committed, live slot not yet touched */
    disk[(FS_JOURNAL_LBA + 3) * 512 + 7] ^= 0x01;
    CHECK(fs_read_slot(2, (char *)got, 4096, &outlen) == 1 && outlen == 4096 && memcmp(got, A, 4096) == 0, "a journal with damaged staged data is discarded; the live file stays as it was");
    { u8 *j = &disk[FS_JOURNAL_LBA * 512]; int z = 1; for (int i = 0; i < 512; i++) if (j[i]) z = 0; CHECK(z, "...and the bad journal is cleared"); }
}

static void test_prog_atomic(void) {
    static u8 A[24576], B[20000], got[24576];
    fill_pat(A, 24576, 3); fill_pat(B, 20000, 99);
    disk_reset();
    CHECK(prog_save_slot(1, "A.MWP", A, 24576, 0) == 1 && prog_load_slot(1, got, 24576) == 24576 && memcmp(got, A, 24576) == 0, "a max-size program saves and loads");
    { u8 *h = &disk[prog_slot_header_lba(1) * 512]; CHECK(*(u32 *)(h + FS_HDR_CRC_OFF) == ref_crc32(A, 24576), "program header carries the right CRC-32"); }
    memcpy(snap_disk, disk, sizeof disk);
    static u8 big[24577];
    CHECK(prog_save_slot(1, "BIG.MWP", big, 24577, 0) == 0 && memcmp(snap_disk, disk, sizeof disk) == 0, "a 24577-byte program is refused and the disk is untouched");
    disk[prog_slot_data_lba(1) * 512 + 9000] ^= 0x80;
    CHECK(prog_load_slot(1, got, 24576) == 0, "a program with a flipped bit is never loaded (CRC mismatch)");

    int bad = 0;
    for (int k = 0; k <= 110; k += 1) {
        disk_reset(); prog_save_slot(2, "A.MWP", A, 24576, 0);
        sector_writes_done = 0; cut_after = k; int ok = prog_save_slot(2, "B.MWP", B, 20000, 16); reboot();
        u32 len = prog_load_slot(2, got, 24576); u32 entry = 0; char nm[PROG_NAME_MAXLEN]; prog_check_slot(2, 0, &entry, nm);
        int is_old = len == 24576 && memcmp(got, A, 24576) == 0 && entry == 0 && nm[0] == 'A';
        int is_new = len == 20000 && memcmp(got, B, 20000) == 0 && entry == 16 && nm[0] == 'B';
        if (!(is_old || is_new)) { bad++; printf("  program: power cut after %d writes => neither old nor new (len %u)\n", k, len); }
        if (k < 50)  CHECK(is_old && !ok, "program: cut after %d writes (before commit): old program intact", k);
        if (k >= 50) CHECK(is_new, "program: cut after %d writes (after commit): new program, completed by recovery", k);
        if (k >= 101) CHECK(ok == 1, "program: save succeeds with %d writes available", k);
    }
    CHECK(bad == 0, "no program power-cut point produced a corrupt or mixed program");
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
    test_rst();
    test_arp();
    test_dns();
    test_dhcp();
    test_tls();
    test_fs_atomic();
    test_prog_atomic();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
