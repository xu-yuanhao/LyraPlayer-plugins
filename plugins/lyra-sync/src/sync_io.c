// sync_io.c - POSIX/Win32 file and socket backends.
#include "sync_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <direct.h>
#pragma comment(lib,"ws2_32.lib")
typedef SSIZE_T ssize_t;
#define SHUT_RDWR SD_BOTH
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <net/if.h>
#include <dirent.h>
#endif

#ifndef _WIN32
static int last_net_err(void){ return errno; }
#else
static int last_net_err(void){ return WSAGetLastError(); }
#endif

#ifdef SYNC_TEST_CLOCK
// host-test hook: tests shift the plugin-visible wall clock to exercise
// timeouts (pairing gate) without real waits. Never compiled for Android.
int64_t g_sync_clock_shift_ms = 0;
#endif
uint64_t sync_now_ms(void) {
#ifdef _WIN32
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime<<32)|ft.dwLowDateTime;
    t = t/10000ULL - 11644473600000ULL;
#else
    struct timespec ts; clock_gettime(CLOCK_REALTIME,&ts);
    uint64_t t = (uint64_t)ts.tv_sec*1000 + ts.tv_nsec/1000000;
#endif
#ifdef SYNC_TEST_CLOCK
    return (uint64_t)((int64_t)t + g_sync_clock_shift_ms);
#else
    return t;
#endif
}
void sync_sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    usleep((useconds_t)ms*1000);
#endif
}

uint64_t sync_monotonic_ms(void) {
#ifdef _WIN32
    return (uint64_t)(GetTickCount64());
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (uint64_t)ts.tv_sec*1000 + ts.tv_nsec/1000000;
#endif
}

// ---------------- files ----------------
#ifdef _WIN32
#define SYNC_PATH_MAX 1024
static void to_wide(const char *u, wchar_t *w, int cap) {
    MultiByteToWideChar(CP_UTF8,0,u,-1,w,cap);
}
int sync_mkdirs(const char *path) {
    wchar_t w[SYNC_PATH_MAX]; to_wide(path,w,SYNC_PATH_MAX);
    // create each prefix
    for (wchar_t *p=w+1; *p; p++) {
        if (*p==L'\\' || *p==L'/') {
            wchar_t save=*p; *p=0;
            CreateDirectoryW(w,NULL);
            *p=save;
        }
    }
    CreateDirectoryW(w,NULL);
    DWORD a=GetFileAttributesW(w);
    return (a!=INVALID_FILE_ATTRIBUTES && (a&FILE_ATTRIBUTE_DIRECTORY))?0:-1;
}
int sync_file_size(const char *path, uint64_t *out) {
    wchar_t w[SYNC_PATH_MAX]; to_wide(path,w,SYNC_PATH_MAX);
    HANDLE h=CreateFileW(w,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                         NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if (h==INVALID_HANDLE_VALUE) return -1;
    LARGE_INTEGER sz; int ok=GetFileSizeEx(h,&sz);
    CloseHandle(h);
    if (!ok) return -1;
    *out=(uint64_t)sz.QuadPart; return 0;
}
int sync_unlink(const char *path) {
    wchar_t w[SYNC_PATH_MAX]; to_wide(path,w,SYNC_PATH_MAX);
    return DeleteFileW(w)?0:-1;
}
int sync_rename(const char *from, const char *to) {
    wchar_t a[SYNC_PATH_MAX],b[SYNC_PATH_MAX];
    to_wide(from,a,SYNC_PATH_MAX); to_wide(to,b,SYNC_PATH_MAX);
    return MoveFileExW(a,b,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)?0:-1;
}
int sync_ftruncate(sync_fd_t fd, uint64_t size) {
    LARGE_INTEGER li; li.QuadPart=(LONGLONG)size;
    return SetFilePointer((HANDLE)fd,li.LowPart,&li.HighPart,FILE_BEGIN)!=INVALID_SET_FILE_POINTER
        && SetEndOfFile((HANDLE)fd)?0:-1;
}
int sync_fsync(sync_fd_t fd) { return FlushFileBuffers((HANDLE)fd)?0:-1; }
// Windows: directory handles need FILE_FLAG_BACKUP_SEMANTICS and
// FlushFileBuffers on them is rejected — dir-entry durability is unverifiable
// on this platform (best effort; never reported as durable success).
int sync_dir_fsync(const char *dirpath) {
    (void)dirpath;
    return -2;
}
int sync_pwrite(sync_fd_t fd, const void *buf, size_t n, uint64_t off) {
    LARGE_INTEGER li; li.QuadPart=(LONGLONG)off;
    if (!SetFilePointerEx((HANDLE)fd,li,NULL,FILE_BEGIN)) return -1;
    DWORD done=0;
    return WriteFile((HANDLE)fd,buf,(DWORD)n,&done,NULL)&&done==n?0:-1;
}
int sync_pread(sync_fd_t fd, void *buf, size_t n, uint64_t off) {
    LARGE_INTEGER li; li.QuadPart=(LONGLONG)off;
    if (!SetFilePointerEx((HANDLE)fd,li,NULL,FILE_BEGIN)) return -1;
    DWORD done=0;
    return ReadFile((HANDLE)fd,buf,(DWORD)n,&done,NULL)?(int)done:-1;
}
int sync_fopen_read(const char *path, sync_fd_t *fd) {
    wchar_t w[SYNC_PATH_MAX]; to_wide(path,w,SYNC_PATH_MAX);
    HANDLE h=CreateFileW(w,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                         NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if (h==INVALID_HANDLE_VALUE) return -1;
    *fd=(sync_fd_t)h; return 0;
}
int sync_fopen_write(const char *path, sync_fd_t *fd) {
    wchar_t w[SYNC_PATH_MAX]; to_wide(path,w,SYNC_PATH_MAX);
    HANDLE h=CreateFileW(w,GENERIC_WRITE,FILE_SHARE_READ,
                         NULL,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if (h==INVALID_HANDLE_VALUE) return -1;
    *fd=(sync_fd_t)h; return 0;
}
void sync_fclose(sync_fd_t fd) { CloseHandle((HANDLE)fd); }
int sync_dir_free_bytes(const char *path, uint64_t *out) {
    wchar_t w[SYNC_PATH_MAX]; to_wide(path,w,SYNC_PATH_MAX);
    ULARGE_INTEGER freeb;
    if (!GetDiskFreeSpaceExW(w,NULL,NULL,&freeb)) return -1;
    *out=(uint64_t)freeb.QuadPart; return 0;
}
// Desktop test builds only. Android host MUST use sync_platform_app_data_dir().
#ifndef SYNC_HOST_TEST
int sync_data_dir(char *out, int cap) {(void)out;(void)cap; return -1;}
#else
int sync_data_dir(char *out, int cap) {
    const char *d=getenv("LYRA_SYNC_DATA");
    if (!d) d=".";
    return snprintf(out,(size_t)cap,"%s",d)<cap?0:-1;
}
#endif
#else
int sync_mkdirs(const char *path) {
    char tmp[1024]; snprintf(tmp,sizeof tmp,"%s",path);
    size_t l=strlen(tmp);
    if (l && tmp[l-1]=='/') tmp[l-1]=0;
    for (char *p=tmp+1; *p; p++) {
        if (*p=='/') { *p=0; mkdir(tmp,0755); *p='/'; }
    }
    return mkdir(tmp,0755)==0||errno==EEXIST?0:-1;
}
int sync_file_size(const char *path, uint64_t *out) {
    struct stat st;
    if (stat(path,&st)) return -1;
    *out=(uint64_t)st.st_size; return 0;
}
int sync_unlink(const char *path) { return unlink(path); }
int sync_rename(const char *from, const char *to) { return rename(from,to); }
int sync_ftruncate(sync_fd_t fd, uint64_t size) { return ftruncate(fd,(off_t)size); }
int sync_fsync(sync_fd_t fd) { return fdatasync(fd); }
// POSIX/Android: open the directory (O_DIRECTORY|O_RDONLY) and fsync it —
// this is what makes a renamed directory entry durable on ext4/f2fs.
// open() failure on a POSIX platform is a REAL problem (directory removed,
// permission lost mid-transfer): -1 => the caller must NOT report the file
// as committed. -2 is reserved for platforms that provably cannot open
// directory handles at all (Windows build), never for POSIX open failures.
int sync_dir_fsync(const char *dirpath) {
#ifndef _WIN32
    int dfd = open(dirpath, O_RDONLY | O_DIRECTORY);
    if (dfd < 0) return -1;   // POSIX: open failure = durability unverified
                              // under a real failure mode => commit fails
    if (fsync(dfd)) { close(dfd); return -1; }
    close(dfd);
    return 0;
#else
    (void)dirpath;
    return -2;
#endif
}
int sync_pwrite(sync_fd_t fd, const void *buf, size_t n, uint64_t off) {
    return pwrite(fd,buf,n,(off_t)off)==(ssize_t)n?0:-1;
}
int sync_pread(sync_fd_t fd, void *buf, size_t n, uint64_t off) {
    return (int)pread(fd,buf,n,(off_t)off);
}
int sync_fopen_read(const char *path, sync_fd_t *fd) {
    int f=open(path,O_RDONLY);
    if (f<0) return -1;
    *fd=f; return 0;
}
int sync_fopen_write(const char *path, sync_fd_t *fd) {
    int f=open(path,O_RDWR|O_CREAT,0644);
    if (f<0) return -1;
    *fd=f; return 0;
}
void sync_fclose(sync_fd_t fd) { close(fd); }
int sync_dir_free_bytes(const char *path, uint64_t *out) {
    (void)path; (void)out; return -1; // statvfs on Android; wired at integration
}
int sync_data_dir(char *out, int cap) {
    const char *d=getenv("LYRA_SYNC_DATA");
    if (!d) d=".";
    return snprintf(out,(size_t)cap,"%s",d)<cap?0:-1;
}
#endif

// ---------------- sockets ----------------
#ifdef _WIN32
static int net_started=0;
static int net_start(void) {
    if (!net_started) {
        WSADATA d;
        if (WSAStartup(MAKEWORD(2,2),&d)) return -1;
        net_started=1;
    }
    return 0;
}
#endif

int sync_tcp_listen(const char *port, sync_fd_t *lfd) {
#ifdef _WIN32
    if (net_start()) return -1;
#endif
    struct addrinfo hints,*res;
    memset(&hints,0,sizeof hints);
    hints.ai_family=AF_INET6;
    hints.ai_socktype=SOCK_STREAM;
    hints.ai_flags=AI_PASSIVE|AI_NUMERICHOST|AI_NUMERICSERV;
    if (getaddrinfo(NULL,port,&hints,&res)) {
        hints.ai_family=AF_INET;
        if (getaddrinfo(NULL,port,&hints,&res)) return -1;
    }
    sync_fd_t s=socket(res->ai_family,res->ai_socktype,res->ai_protocol);
    if (s==SYNC_FD_INVALID){ freeaddrinfo(res); return -1; }
    int off=0;
#ifdef _WIN32
    setsockopt(s,IPPROTO_IPV6,IPV6_V6ONLY,(const char*)&off,sizeof off);
#else
    setsockopt(s,IPPROTO_IPV6,IPV6_V6ONLY,&off,sizeof off);
#endif
    int on=1;
#ifdef _WIN32
    setsockopt(s,SOL_SOCKET,SO_REUSEADDR,(const char*)&on,sizeof on);
    if (bind(s,res->ai_addr,(int)res->ai_addrlen)||listen(s,4)){ freeaddrinfo(res); sync_close(s); return -1; }
#else
    setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&on,sizeof on);
    if (bind(s,res->ai_addr,res->ai_addrlen)||listen(s,4)){ freeaddrinfo(res); close(s); return -1; }
#endif
    freeaddrinfo(res);
    *lfd=s; return 0;
}
int sync_tcp_accept(sync_fd_t lfd, sync_fd_t *cfd, char *ip, int ipcap) {
    struct sockaddr_storage ss;
#ifdef _WIN32
    int sl=(int)sizeof ss;
#else
    socklen_t sl=sizeof ss;
#endif
    sync_fd_t s=accept(lfd,(struct sockaddr*)&ss,&sl);
    if (s==SYNC_FD_INVALID) return -1;
    if (ip) {
        void *addr = ss.ss_family==AF_INET6 ? (void*)&((struct sockaddr_in6*)&ss)->sin6_addr
                                            : (void*)&((struct sockaddr_in*)&ss)->sin_addr;
        if (!inet_ntop(ss.ss_family,addr,ip,(socklen_t)ipcap)) snprintf(ip,(size_t)ipcap,"?");
    }
    *cfd=s; return 0;
}
int sync_tcp_connect(const char *ip, const char *port, sync_fd_t *cfd, int timeout_ms) {
#ifdef _WIN32
    if (net_start()) return -1;
#endif
    struct addrinfo hints,*res;
    memset(&hints,0,sizeof hints);
    hints.ai_family=AF_UNSPEC; hints.ai_socktype=SOCK_STREAM;
    hints.ai_flags=AI_NUMERICHOST|AI_NUMERICSERV;
    if (getaddrinfo(ip,port,&hints,&res)) return -1;
    sync_fd_t s=socket(res->ai_family,res->ai_socktype,res->ai_protocol);
    if (s==SYNC_FD_INVALID){ freeaddrinfo(res); return -1; }
#ifdef _WIN32
    u_long nb=1; ioctlsocket(s,FIONBIO,&nb);
    connect(s,res->ai_addr,(int)res->ai_addrlen);
    fd_set w; FD_ZERO(&w); FD_SET(s,&w);
    struct timeval tv; tv.tv_sec=timeout_ms/1000; tv.tv_usec=(timeout_ms%1000)*1000;
    int ok=select((int)s+1,NULL,&w,NULL,timeout_ms<0?NULL:&tv);
    if (ok<=0){ freeaddrinfo(res); sync_close(s); return -1; }
    int err=0; int el=sizeof err;
    getsockopt(s,SOL_SOCKET,SO_ERROR,(char*)&err,&el);
    nb=0; ioctlsocket(s,FIONBIO,&nb);
    if (err){ freeaddrinfo(res); sync_close(s); return -1; }
#else
    int flags=fcntl(s,F_GETFL,0);
    fcntl(s,F_SETFL,flags|O_NONBLOCK);
    connect(s,res->ai_addr,res->ai_addrlen);
    fd_set w; FD_ZERO(&w); FD_SET(s,&w);
    struct timeval tv; tv.tv_sec=timeout_ms/1000; tv.tv_usec=(timeout_ms%1000)*1000;
    int ok=select(s+1,NULL,&w,NULL,timeout_ms<0?NULL:&tv);
    if (ok<=0){ freeaddrinfo(res); close(s); return -1; }
    int err=0; socklen_t el=sizeof err;
    getsockopt(s,SOL_SOCKET,SO_ERROR,&err,&el);
    fcntl(s,F_SETFL,flags);
    if (err){ freeaddrinfo(res); close(s); return -1; }
#endif
    freeaddrinfo(res);
    *cfd=s; return 0;
}
int sync_send_all(sync_fd_t fd, const void *buf, size_t n) {
    const char *p=(const char*)buf;
    while (n) {
#ifdef _WIN32
        int r=send(fd,p,(int)n,0);
        if (r==SOCKET_ERROR) return -1;
#else
        ssize_t r=send(fd,p,n,MSG_NOSIGNAL);
        if (r<0){ if (errno==EINTR) continue; return -1; }
#endif
        p+=r; n-=(size_t)r;
    }
    return 0;
}
int sync_recv_all(sync_fd_t fd, void *buf, size_t n) {
    char *p=(char*)buf;
    while (n) {
#ifdef _WIN32
        int r=recv(fd,p,(int)n,0);
        if (r==0) return n==(size_t)0?1:0;   // peer closed
        if (r==SOCKET_ERROR){ if (last_net_err()==WSAEINTR) continue; return -1; }
#else
        ssize_t r=recv(fd,p,n,0);
        if (r==0) return 0;                  // peer closed mid-stream
        if (r<0){ if (errno==EINTR) continue; return -1; }
#endif
        p+=r; n-=(size_t)r;
    }
    return 1;
}
// Force-wake a peer blocked in accept()/recv() on this socket during shutdown.
int sync_shutdown_fd(sync_fd_t fd) {
#ifdef _WIN32
    return shutdown(fd,SD_BOTH);
#else
    return shutdown(fd,SHUT_RDWR);
#endif
}
void sync_close(sync_fd_t fd) {
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}
int sync_settimeo(sync_fd_t fd, int send_ms, int recv_ms) {
#ifdef _WIN32
    DWORD sv=(DWORD)send_ms, rv=(DWORD)recv_ms; // Winsock takes DWORD milliseconds
    if (setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,(const char*)&sv,sizeof sv)) return -1;
    if (setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,(const char*)&rv,sizeof rv)) return -1;
#else
    struct timeval sv; sv.tv_sec=send_ms/1000; sv.tv_usec=(send_ms%1000)*1000;
    struct timeval rv; rv.tv_sec=recv_ms/1000; rv.tv_usec=(recv_ms%1000)*1000;
    if (setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&sv,sizeof sv)) return -1;
    if (setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&rv,sizeof rv)) return -1;
#endif
    return 0;
}
int sync_nodelay(sync_fd_t fd, int on) {
#ifdef _WIN32
    return setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,(const char*)&on,sizeof on);
#else
    return setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&on,sizeof on);
#endif
}
// MSG_PEEK probe used by the pairing-confirm wait loop: detects the peer
// hanging up without consuming anything from the stream.
int sync_recv_peek(sync_fd_t fd) {
    uint8_t b;
#ifdef _WIN32
    int r=recv(fd,(char*)&b,1,MSG_PEEK);
    if (r==SOCKET_ERROR) return last_net_err()==WSAEWOULDBLOCK?1:-1;
#else
    ssize_t r=recv(fd,&b,1,MSG_PEEK);
    if (r<0) return (errno==EAGAIN||errno==EWOULDBLOCK)?1:-1;
#endif
    return r>0?1:0;
}
// Real local IPv4 for display, network-scenario aware:
//  - 手机热点/Wi-Fi/蜂窝并存：connect() 探测只会命中默认路由（蜂窝或 VPN），
//    对端不在那个网段。改为 getifaddrs 枚举全部 IPv4，排除 loopback，
//    按"最可能是局域网对端可达"排序：192.168.* > 172.16-31.* > 10.* >
//    其他私有 > 其余（蜂窝公网地址）。UI 把候选列出来，用户选择与对端
//    同一网络的那个；单结果时直接显示。
//  - VPN tun 接口地址（常见 10.x/198.18.x）也被列出但排后，并保留接口名
//    供用户辨认（wlan0/rmnet/tun0）。
//  - 无互联网 Wi-Fi（仅 AP 模式连接）：接口仍持有 IPv4 且会被枚举到——
//    正确行为，同步不依赖互联网。
// 返回 0：out = "addr|ifname"（首个最佳候选）；-1：无 IPv4（显式报错）。
// 完整候选列表经 sync_local_ip_list 获取。
#ifndef _WIN32
#include <ifaddrs.h>
#endif
int sync_local_ip(char *out, int cap) {
    char list[512];
    int n=sync_local_ip_list(list,(int)sizeof list);
    if (n<=0) return -1;
    // list lines are "addr|ifname"; return the PURE ADDRESS only — an
    // ifname inside a connection address or pairing payload would be
    // unparseable garbage (inet_pton fails, peer connects to nothing).
    const char *line=list;
    const char *bar=strchr(line,'|');
    size_t take=bar?(size_t)(bar-line):strlen(line);
    if ((int)take>=cap) take=(size_t)(cap-1);
    memcpy(out,line,take); out[take]=0;
    return 0;
}
// Candidate list: one "addr|ifname" per line, best-first. cap-bounded.
// ---- cross-platform candidate machinery (unit-testable) ----
// SyncIpCand declared in sync_io.h. Interface-class weights. CATEGORIES are guaranteed non-overlapping bands
// (each category owns a distinct score range), so ordering by score == the
// documented category order regardless of individual values:
//   100-199  LAN-capable (wlan/ap/eth), sub-ranked by RFC1918 class inside
//   200-299  unknown interface (address-range heuristic only)
//   300-399  VPN tunnel (tun/wg/ppp)
//   400-499  cellular (rmnet/ccmni/pdp/usb)
int sync_ip_score(const char *ifn, const char *addr) {
    if (!strncmp(ifn,"rmnet",5) || !strncmp(ifn,"ccmni",5) ||
        !strncmp(ifn,"pdp",3)  || !strncmp(ifn,"usb",3))
        return 400;
    if (!strncmp(ifn,"tun",3) || !strncmp(ifn,"wg",2) || !strncmp(ifn,"ppp",3))
        return 300;
    if (!strncmp(ifn,"wlan",4) || !strncmp(ifn,"ap",2) ||
        !strncmp(ifn,"swlan",5) || !strncmp(ifn,"eth",3)) {
        unsigned a[4]; if (sscanf(addr,"%u.%u.%u.%u",&a[0],&a[1],&a[2],&a[3])!=4)
            return 190;                    // unparseable on a LAN iface: last
        if (a[0]==192 && a[1]==168) return 100;                   // 192.168/16
        if (a[0]==172 && a[1]>=16 && a[1]<=31) return 110;        // 172.16/12
        if (a[0]==10) return 120;                                 // 10/8
        return 190;                                               // other
    }
    // unknown interface: address-range heuristic only, below known-LAN
    unsigned a[4]; if (sscanf(addr,"%u.%u.%u.%u",&a[0],&a[1],&a[2],&a[3])!=4)
        return 290;
    if (a[0]==192 && a[1]==168) return 200;
    if (a[0]==172 && a[1]>=16 && a[1]<=31) return 210;
    if (a[0]==10) return 220;
    return 290;
}
// Stable insertion sort by score; ties keep discovery order (no overlap:
// categories are disjoint score bands, so equal scores only occur within
// one category where relative order is arbitrary-but-stable).
void sync_ip_sort(SyncIpCand *c, int *score, int n) {
    for (int i=1;i<n;i++) {
        int j=i;
        while (j>0 && score[j-1]>score[j]) {
            SyncIpCand tc; memcpy(&tc,&c[j-1],sizeof tc);
            memcpy(&c[j-1],&c[j],sizeof tc); memcpy(&c[j],&tc,sizeof tc);
            int ts=score[j-1]; score[j-1]=score[j]; score[j]=ts;
            j--;
        }
    }
}
// Serialize: "addr|ifname" per line (pure addr extraction happens in
// sync_local_ip; the ifname suffix is list-format only).
int sync_ip_emit(const SyncIpCand *c, int n, char *out, int cap) {
    int used=0;
    for (int i=0;i<n && used<cap-2;i++) {
        int w=snprintf(out+used,(size_t)(cap-used),"%s|%s%s",
                       c[i].addr,c[i].ifn,i+1<n?"\n":"");
        if (w<0 || w>=cap-used) break;
        used+=w;
    }
    return n;
}
// ---- platform enumeration: thin wrapper over getifaddrs (POSIX) ----
int sync_local_ip_list(char *out, int cap) {
#ifndef _WIN32
    struct ifaddrs *ifa0=NULL;
    if (getifaddrs(&ifa0)) return -1;
    SyncIpCand c[16]; int score[16]; int n=0;
    for (struct ifaddrs *ifa=ifa0; ifa && n<16; ifa=ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family!=AF_INET) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;
        if (!(ifa->ifa_flags & IFF_UP)) continue;
        char ip[64];
        if (!inet_ntop(AF_INET,&((struct sockaddr_in*)ifa->ifa_addr)->sin_addr,
                       ip,(socklen_t)sizeof ip)) continue;
        struct sockaddr_in chk; memset(&chk,0,sizeof chk);
        if (!inet_pton(AF_INET,ip,&chk.sin_addr)) continue;   // never emit
                                                              // unparseable
        snprintf(c[n].addr,sizeof c[n].addr,"%s",ip);
        snprintf(c[n].ifn,sizeof c[n].ifn,"%s",ifa->ifa_name);
        score[n]=sync_ip_score(c[n].ifn,ip);
        n++;
    }
    freeifaddrs(ifa0);
    if (!n) return -1;
    sync_ip_sort(c,score,n);
    return sync_ip_emit(c,n,out,cap);
#else
    (void)out; (void)cap;
    return -1;   // Windows: dir/IP enumeration branch not implemented (-2
                 // semantics reserved; see sync_dir_fsync) — NOT a network
                 // measurement result.
#endif
}
int sync_udp_beacon_open(const char *port, sync_fd_t *fd) {
#ifdef _WIN32
    if (net_start()) return -1;
#endif
    struct addrinfo hints,*res;
    memset(&hints,0,sizeof hints);
    hints.ai_family=AF_INET6; hints.ai_socktype=SOCK_DGRAM;
    hints.ai_flags=AI_PASSIVE|AI_NUMERICSERV;
    if (getaddrinfo(NULL,port,&hints,&res)) {
        hints.ai_family=AF_INET;
        if (getaddrinfo(NULL,port,&hints,&res)) return -1;
    }
    sync_fd_t s=socket(res->ai_family,res->ai_socktype,res->ai_protocol);
    if (s==SYNC_FD_INVALID){ freeaddrinfo(res); return -1; }
    int off=0, on=1;
#ifdef _WIN32
    setsockopt(s,IPPROTO_IPV6,IPV6_V6ONLY,(const char*)&off,sizeof off);
    setsockopt(s,SOL_SOCKET,SO_REUSEADDR,(const char*)&on,sizeof on);
    if (bind(s,res->ai_addr,(int)res->ai_addrlen)){ freeaddrinfo(res); sync_close(s); return -1; }
#else
    setsockopt(s,IPPROTO_IPV6,IPV6_V6ONLY,&off,sizeof off);
    setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&on,sizeof on);
    if (bind(s,res->ai_addr,res->ai_addrlen)){ freeaddrinfo(res); close(s); return -1; }
#endif
    freeaddrinfo(res);
    *fd=s; return 0;
}
int sync_udp_sendto(sync_fd_t fd, const char *ip, const char *port,
                    const void *buf, size_t n) {
    struct addrinfo hints,*res;
    memset(&hints,0,sizeof hints);
    hints.ai_family=AF_UNSPEC; hints.ai_socktype=SOCK_DGRAM;
    hints.ai_flags=AI_NUMERICHOST|AI_NUMERICSERV;
    if (getaddrinfo(ip,port,&hints,&res)) return -1;
#ifdef _WIN32
    int r=(int)sendto(fd,(const char*)buf,(int)n,0,res->ai_addr,(int)res->ai_addrlen);
#else
    ssize_t r=sendto(fd,buf,n,0,res->ai_addr,res->ai_addrlen);
#endif
    freeaddrinfo(res);
    return r<(int)n?-1:0;
}
int sync_udp_recvfrom(sync_fd_t fd, void *buf, size_t cap, char *ip, int ipcap) {
    struct sockaddr_storage ss;
#ifdef _WIN32
    int sl=(int)sizeof ss;
    int r=recvfrom(fd,(char*)buf,(int)cap,0,(struct sockaddr*)&ss,&sl);
    if (r==SOCKET_ERROR) return -1;
#else
    socklen_t sl=sizeof ss;
    ssize_t r=recvfrom(fd,buf,cap,0,(struct sockaddr*)&ss,&sl);
    if (r<0) return -1;
#endif
    if (ip) {
        void *addr = ss.ss_family==AF_INET6 ? (void*)&((struct sockaddr_in6*)&ss)->sin6_addr
                                            : (void*)&((struct sockaddr_in*)&ss)->sin_addr;
        if (!inet_ntop(ss.ss_family,addr,ip,(socklen_t)ipcap)) snprintf(ip,(size_t)ipcap,"?");
    }
    return (int)r;
}
