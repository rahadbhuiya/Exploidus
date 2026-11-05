#include "table.h"
#include "../net/net.h"
#include "../net/socket/socket.h"
#include "../net/dns/dns.h"
#include "../cnsl/cnsl.h"
#include "../cap/broker.h"
#include "../proc/scheduler.h"
#include "../drivers/serial.h"
#include <string.h>

int icmp_send_echo(uint32_t dst, uint16_t seq);


static bool net_cap_check(syscall_frame_t *f, uint64_t required_rights)
{
    if (!g_current_proc) return false;
    cap_token_t tok = { f->rdi, f->rsi };
    if (tok.upper == 0 && tok.lower == 0) return true;
    return broker_check(g_current_proc->pid, tok, required_rights);
}



int64_t sys_socket(syscall_frame_t *f)
{
    if (!net_cap_check(f, CAP_RIGHT_NET_SEND | CAP_RIGHT_NET_RECV)) return -1;
    sock_type_t type = (sock_type_t)f->rdx;
    return (int64_t)net_socket(type);
}


int64_t sys_bind(syscall_frame_t *f)
{
    if (!net_cap_check(f, CAP_RIGHT_NET_BIND)) return -1;
    int sock_fd = (int)(int64_t)f->rdx; uint16_t port = (uint16_t)f->r10;
    return (int64_t)net_bind(sock_fd, port);
}


int64_t sys_connect(syscall_frame_t *f)
{
    if (!net_cap_check(f, CAP_RIGHT_NET_SEND)) return -1;
    int sock_fd = (int)(int64_t)f->rdx; ip4_t ip = (ip4_t)f->r10; uint16_t port = (uint16_t)f->r8;
    return (int64_t)net_connect(sock_fd, ip, port);
}


int64_t sys_listen(syscall_frame_t *f)
{
    if (!net_cap_check(f, CAP_RIGHT_NET_BIND)) return -1;
    return (int64_t)net_listen((int)(int64_t)f->rdx);
}


int64_t sys_accept(syscall_frame_t *f)
{
    if (!net_cap_check(f, CAP_RIGHT_NET_RECV)) return -1;
    return (int64_t)net_accept((int)(int64_t)f->rdx);
}


int64_t sys_net_send(syscall_frame_t *f)
{
    if (!net_cap_check(f, CAP_RIGHT_NET_SEND)) return -1;
    int sock_fd = (int)(int64_t)f->rdx; const void *buf = (const void *)(uintptr_t)f->r10; uint16_t len = (uint16_t)f->r8;
    if (!uptr_ok(f->r10, len)) return -1;
    return (int64_t)net_send(sock_fd, buf, len);
}


int64_t sys_net_recv(syscall_frame_t *f)
{
    if (!net_cap_check(f, CAP_RIGHT_NET_RECV)) return -1;
    int sock_fd = (int)(int64_t)f->rdx; void *buf = (void *)(uintptr_t)f->r10; uint16_t len = (uint16_t)f->r8;
    if (!uptr_ok(f->r10, len)) return -1;
    return (int64_t)net_recv(sock_fd, buf, len);
}


int64_t sys_net_close(syscall_frame_t *f)
{
    net_socket_close((int)(int64_t)f->rdx); return 0;
}


int64_t sys_getifaddr(syscall_frame_t *f)
{
    (void)f; netif_t *iface = netif_default(); if (!iface) return -1; return (int64_t)iface->ip;
}


int64_t sys_ping(syscall_frame_t *f)
{
    uint32_t ip = (uint32_t)f->rdi;
    serial_print("[PING] sys_ping called ip="); serial_printhex((uint64_t)ip); serial_print("\n");
    static uint16_t seq = 0;
    __asm__ volatile("sti");
    int64_t r = (int64_t)icmp_send_echo(ip, seq++);
    __asm__ volatile("cli");
    return r;
}


int64_t sys_cnsl_unblock(syscall_frame_t *f)
{ bool ok = cnsl_unblock((uint32_t)f->rdi); return ok ? 0 : -1; }


int64_t sys_cnsl_block_ttl(syscall_frame_t *f)
{ return (int64_t)cnsl_get_block_ttl((uint32_t)f->rdi); }


int64_t sys_cnsl_list(syscall_frame_t *f)
{
    uint64_t max = f->rsi; if (max == 0) return 0;
    if (max > USER_ADDR_MAX / sizeof(cnsl_list_entry_t)) return -1;
    uint64_t bytes = max * sizeof(cnsl_list_entry_t);
    if (!uptr_ok(f->rdi, bytes)) return -1;
    return (int64_t)cnsl_list((cnsl_list_entry_t *)(uintptr_t)f->rdi, (uint16_t)max);
}



int64_t sys_http_get(syscall_frame_t *f)
{
    if (!uptr_ok(f->rdi, 1))      return -1;
    if (!uptr_ok(f->rsi, f->rdx)) return -1;
    const char *url = (const char *)(uintptr_t)f->rdi;
    uint8_t *buf = (uint8_t *)(uintptr_t)f->rsi;
    uint64_t bsz = f->rdx;
    if (url[0]!='h'||url[1]!='t'||url[2]!='t'||url[3]!='p') return -2;
    const char *host_start = url + 7;
    const char *slash = host_start;
    while (*slash && *slash != '/' && *slash != ':') slash++;
    if (!*slash) return -3;
    char host[64]; uint32_t hlen = (uint32_t)(slash - host_start);
    if (hlen >= 64) return -4;
    for (uint32_t i = 0; i < hlen; i++) host[i] = host_start[i];
    host[hlen] = 0;
    uint16_t port = 80; const char *path = slash;
    if (*slash == ':') { slash++; port = 0; while (*slash>='0'&&*slash<='9') port=port*10+(*slash++-'0'); path=slash; }
    if (!*path) path = "/";
    ip4_t ip = 0; const char *p = host; int dots = 0;
    for (const char *t=host;*t;t++) if(*t=='.') dots++;
    if (dots==3) {
        bool all_digits=true; for(const char *t=host;*t;t++) if(*t!='.'&&(*t<'0'||*t>'9')){all_digits=false;break;}
        if (all_digits) { for(int i=0;i<4;i++){uint8_t oct=0;while(*p>='0'&&*p<='9')oct=(uint8_t)(oct*10+(*p++-'0'));ip=(ip<<8)|oct;if(*p=='.')p++;} }
    }
    if (!ip) { ip=dns_resolve(host); if(!ip) return -5; }
    int sock=(int)net_socket(SOCK_TCP); if(sock<0) return -6;
    if(net_connect(sock,ip,port)<0){net_socket_close(sock);return -7;}
    char req[512]; uint32_t rlen=0;
#define REQ_APPEND(s) do { \
        for (const char *_s = (s); *_s; _s++) { \
            if (rlen >= sizeof(req) - 1) { net_socket_close(sock); return -9; } \
            req[rlen++] = *_s; \
        } \
    } while (0)
    REQ_APPEND("GET ");
    REQ_APPEND(path);
    REQ_APPEND(" HTTP/1.0\r\nHost: ");
    REQ_APPEND(host);
    REQ_APPEND("\r\nConnection: close\r\n\r\n");
#undef REQ_APPEND
    net_send(sock,req,(uint16_t)rlen);
    static uint8_t rbuf[4096]; int64_t total=0; int headers_done=0; uint8_t hdr_buf[4]={0,0,0,0}; uint32_t empty_recvs=0;
    char status_line[32]; uint32_t status_len=0; int status_done=0; int http_status=0;
    while(1){
        int64_t n=net_recv(sock,rbuf,sizeof(rbuf));
        if(n<=0){
            empty_recvs++;
            if(empty_recvs>500)break;
            /*
 * timer tick, ...) instead of immediately re-polling —
 * this loop was busy-spinning up to 500 times back-to-back
 * with nothing to yield the CPU, which could make the
 * whole system sluggish for the duration of any HTTP
 */
            sched_yield();
            continue;
        }
        empty_recvs=0;
        for(int64_t i=0;i<n;i++){
            uint8_t c=rbuf[i];
            if(!status_done){
                if(c=='\n'){status_line[status_len]=0;status_done=1;
                    const char *sp=status_line;while(*sp&&*sp!=' ')sp++;while(*sp==' ')sp++;
                    int code=0,digits=0;while(*sp>='0'&&*sp<='9'&&digits<3){code=code*10+(*sp-'0');sp++;digits++;}
                    http_status=code;if(http_status<200||http_status>=300){net_socket_close(sock);return -8;}
                }else if(c!='\r'&&status_len<sizeof(status_line)-1)status_line[status_len++]=(char)c;
            }
            if(!headers_done){hdr_buf[0]=hdr_buf[1];hdr_buf[1]=hdr_buf[2];hdr_buf[2]=hdr_buf[3];hdr_buf[3]=c;
                if(hdr_buf[0]=='\r'&&hdr_buf[1]=='\n'&&hdr_buf[2]=='\r'&&hdr_buf[3]=='\n')headers_done=1;
            }else{if((uint64_t)total<bsz)buf[total++]=c;}
        }
    }
    net_socket_close(sock); return total;
}



int64_t sys_http_download(syscall_frame_t *f)
{
    if (!uptr_ok(f->rdi,1)) return -1;
    if (!uptr_ok(f->rsi,1)) return -1;
    if (f->rdx && !uptr_ok(f->rdx,32)) return -1;
    const char *url=(const char *)(uintptr_t)f->rdi;
    const char *dst_path=(const char *)(uintptr_t)f->rsi;
    uint8_t *hash_out=(uint8_t *)(uintptr_t)f->rdx;
    if(url[0]!='h'||url[1]!='t'||url[2]!='t'||url[3]!='p') return -2;
    const char *host_start = url + 7;
    const char *slash = host_start;
    while (*slash && *slash != '/' && *slash != ':') slash++;
    if (!*slash) return -3;
    char host[64];
    uint32_t hlen = (uint32_t)(slash - host_start);
    if (hlen >= 64) return -4;
    for (uint32_t i = 0; i < hlen; i++) host[i] = host_start[i];
    host[hlen] = 0;
    uint16_t port = 80;
    const char *path = slash;
    if (*slash == ':') {
        slash++;
        port = 0;
        while (*slash >= '0' && *slash <= '9')
            port = (uint16_t)(port * 10 + (*slash++ - '0'));
        path = slash;
    }
    if (!*path) path = "/";
    ip4_t ip = 0;
    const char *pp = host;
    int dots = 0;
    for (const char *t = host; *t; t++) if (*t == '.') dots++;
    if (dots == 3) {
        bool all_digits = true;
        for (const char *t = host; *t; t++)
            if (*t != '.' && (*t < '0' || *t > '9')) { all_digits = false; break; }
        if(all_digits){for(int i=0;i<4;i++){uint8_t oct=0;while(*pp>='0'&&*pp<='9')oct=(uint8_t)(oct*10+(*pp++-'0'));ip=(ip<<8)|oct;if(*pp=='.')pp++;}}}
    if(!ip){ip=dns_resolve(host);if(!ip)return -5;}
    int fd=vfs_open(dst_path,1);
    if(fd<0){int cfd=vfs_create(dst_path,0);if(cfd<0)return -9;vfs_close(cfd);fd=vfs_open(dst_path,1);if(fd<0)return -9;}
    int sock=(int)net_socket(SOCK_TCP); if(sock<0){vfs_close(fd);return -6;}
    if(net_connect(sock,ip,port)<0){vfs_close(fd);net_socket_close(sock);return -7;}
    static char req[512]; uint32_t rlen=0;
#define REQ_APPEND(s) do { \
        for (const char *_s = (s); *_s; _s++) { \
            if (rlen >= sizeof(req) - 1) { vfs_close(fd); net_socket_close(sock); return -10; } \
            req[rlen++] = *_s; \
        } \
    } while (0)
    REQ_APPEND("GET ");
    REQ_APPEND(path);
    REQ_APPEND(" HTTP/1.0\r\nHost: ");
    REQ_APPEND(host);
    REQ_APPEND("\r\nConnection: close\r\n\r\n");
#undef REQ_APPEND
    net_send(sock,req,(uint16_t)rlen);
    static uint8_t rbuf[4096]; int64_t total=0; int headers_done=0; uint8_t hdr_buf[4]={0,0,0,0}; uint32_t empty_recvs=0;
    char status_line[32]; uint32_t status_len=0; int status_done=0; int http_status=0;
    static uint8_t wbuf[4096]; uint32_t wbuf_pos=0;
    blake3_ctx_t hash_ctx; if(hash_out)blake3_init(&hash_ctx);
    while(1){
        int64_t n=net_recv(sock,rbuf,sizeof(rbuf));
        if(n<=0){
            empty_recvs++;
            if(empty_recvs>500)break;
            /* Same fix as sys_http_get: sched_yield() instead of a
             * bare hlt, so this loop doesn't starve other ready
             * processes for the duration of the download. */
            sched_yield();
            continue;
        }
        empty_recvs=0;
        for(int64_t i=0;i<n;i++){
            uint8_t c=rbuf[i];
            if(!status_done){
                if(c=='\n'){status_line[status_len]=0;status_done=1;
                    const char *sp=status_line;while(*sp&&*sp!=' ')sp++;while(*sp==' ')sp++;
                    int code=0,digits=0;while(*sp>='0'&&*sp<='9'&&digits<3){code=code*10+(*sp-'0');sp++;digits++;}
                    http_status=code;if(http_status<200||http_status>=300){vfs_close(fd);net_socket_close(sock);return -8;}
                }else if(c!='\r'&&status_len<sizeof(status_line)-1)status_line[status_len++]=(char)c;
            }
            if(!headers_done){hdr_buf[0]=hdr_buf[1];hdr_buf[1]=hdr_buf[2];hdr_buf[2]=hdr_buf[3];hdr_buf[3]=c;
                if(hdr_buf[0]=='\r'&&hdr_buf[1]=='\n'&&hdr_buf[2]=='\r'&&hdr_buf[3]=='\n')headers_done=1;
            }else{wbuf[wbuf_pos++]=c;
                if(wbuf_pos==sizeof(wbuf)){if(hash_out)blake3_update(&hash_ctx,wbuf,wbuf_pos);vfs_write(fd,wbuf,wbuf_pos);total+=wbuf_pos;wbuf_pos=0;}
            }
        }
    }
    if(wbuf_pos>0){if(hash_out)blake3_update(&hash_ctx,wbuf,wbuf_pos);vfs_write(fd,wbuf,wbuf_pos);total+=wbuf_pos;}
    vfs_close(fd); net_socket_close(sock);
    if(hash_out)blake3_final(&hash_ctx,hash_out);
    return total;
}


