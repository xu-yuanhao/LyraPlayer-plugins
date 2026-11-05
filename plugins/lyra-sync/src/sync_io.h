// sync_io.h - filesystem + socket abstraction over POSIX/Win32.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "sync_common.h"

#ifdef __cplusplus
extern "C" {
#endif
#if defined(_WIN32)
typedef intptr_t sync_fd_t;   // SOCKET or OS handle; INVALID = -1
#define SYNC_FD_INVALID (-1)
#else
typedef int sync_fd_t;
#define SYNC_FD_INVALID (-1)
#endif

// Files
int sync_mkdirs(const char *path);                    // mkdir -p
int sync_file_size(const char *path, uint64_t *out);  // 0 ok
int sync_unlink(const char *path);
int sync_rename(const char *from, const char *to);
int sync_ftruncate(sync_fd_t fd, uint64_t size);
int sync_fsync(sync_fd_t fd);                         // fdatasync/fsync
// Durability of a DIRECTORY ENTRY (rename visibility after power loss).
// Returns 0 = dir entry synced; -1 = durability failure (fsync failed, or on
//   POSIX the directory open failed — caller must NOT report the rename as
//   durable/committed);
// -2 = platform provably cannot open directory handles (Windows build only;
//   rename succeeded but durability is unverifiable — best effort).
// On Android a directory open failure MUST fail the commit: it means the
// authorized directory vanished or permissions were revoked mid-transfer.
int sync_dir_fsync(const char *dirpath);
int sync_pwrite(sync_fd_t fd, const void *buf, size_t n, uint64_t off);
int sync_pread(sync_fd_t fd, void *buf, size_t n, uint64_t off);
int sync_fopen_read(const char *path, sync_fd_t *fd);
int sync_fopen_write(const char *path, sync_fd_t *fd); // O_CREAT, no truncate
void sync_fclose(sync_fd_t fd);
int sync_dir_free_bytes(const char *path, uint64_t *out);

// Sockets (blocking with SO_RCVTIMEO/SO_SNDTIMEO set by caller)
int sync_tcp_listen(const char *port, sync_fd_t *lfd);       // bind+listen, IPv6+IPv4
int sync_tcp_accept(sync_fd_t lfd, sync_fd_t *cfd, char *ip, int ipcap);
int sync_tcp_connect(const char *ip, const char *port, sync_fd_t *cfd, int timeout_ms);
int sync_send_all(sync_fd_t fd, const void *buf, size_t n);
int sync_recv_all(sync_fd_t fd, void *buf, size_t n);       // returns 1 full, 0 eof, -1 err
void sync_close(sync_fd_t fd);
int sync_shutdown_fd(sync_fd_t fd);   // wake blocked accept/recv before close
int sync_settimeo(sync_fd_t fd, int send_ms, int recv_ms);
int sync_nodelay(sync_fd_t fd, int on);
// non-consuming liveness probe: 1 data available, 0 peer closed, -1 err/none
int sync_recv_peek(sync_fd_t fd);

// UDP discovery
int sync_udp_beacon_open(const char *port, sync_fd_t *fd);   // bind for listening
// Local LAN IPv4 for QR/status display. out = "addr|ifname" of the
// best-scoring candidate (192.168 > 172.16/12 > 10/8 VPN/tun > cellular).
// Covers: Wi-Fi+cellular concurrent, hotspot, no-internet Wi-Fi (AP still
// holds an IPv4), VPN (tun listed but ranked after LAN ranges).
// Returns 0 ok, -1 no IPv4 (caller shows explicit error, no placeholder).
int sync_local_ip(char *out, int cap);
// Full candidate list addr|ifname lines, best first (UI can offer a picker).
int sync_local_ip_list(char *out, int cap);

// Cross-platform candidate machinery (unit-testable; no OS calls):
typedef struct SyncIpCand { char addr[64]; char ifn[32]; } SyncIpCand;
// Category score: LAN 100-199 (wlan/ap/eth, RFC1918 sub-ranked),
// unknown 200-299, VPN 300-399, cellular 400-499 — disjoint bands so
// score order == category order.
int sync_ip_score(const char *ifn, const char *addr);
void sync_ip_sort(SyncIpCand *c, int *score, int n);
int sync_ip_emit(const SyncIpCand *c, int n, char *out, int cap);
int sync_udp_sendto(sync_fd_t fd, const char *ip, const char *port,
                    const void *buf, size_t n);
int sync_udp_recvfrom(sync_fd_t fd, void *buf, size_t cap, char *ip, int ipcap);

// Device id / persistence helpers (platform-specific location)
int sync_data_dir(char *out, int cap);   // plugin-private dir for task DB
uint64_t sync_now_ms(void);
void sync_sleep_ms(int ms);
uint64_t sync_monotonic_ms(void);

#ifdef __cplusplus
}
#endif