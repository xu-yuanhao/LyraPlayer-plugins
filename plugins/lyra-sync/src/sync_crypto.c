// sync_crypto.c - SHA-256, HMAC, HKDF, AES-256-GCM, RNG. FIPS 180-4 / SP 800-38D.
#include "sync_crypto.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#ifdef _WIN32
#include <intrin.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};

static uint32_t ror(uint32_t x, int n){ return (x>>n)|(x<<(32-n)); }
#define S0(x) (ror(x,2)^ror(x,13)^ror(x,22))
#define S1(x) (ror(x,6)^ror(x,11)^ror(x,25))
#define s0(x) (ror(x,7)^ror(x,18)^(x>>3))
#define s1(x) (ror(x,17)^ror(x,19)^(x>>10))

static void sha256_block(sha256_ctx *c, const uint8_t *p) {
    uint32_t w[64], a,b,cc,d,e,f,g,h,t1,t2;
    for (int i=0;i<16;i++) w[i]=((uint32_t)p[i*4]<<24)|((uint32_t)p[i*4+1]<<16)|((uint32_t)p[i*4+2]<<8)|p[i*4+3];
    for (int i=16;i<64;i++) w[i]=s1(w[i-2])+w[i-7]+s0(w[i-15])+w[i-16];
    a=c->s[0];b=c->s[1];cc=c->s[2];d=c->s[3];e=c->s[4];f=c->s[5];g=c->s[6];h=c->s[7];
    for (int i=0;i<64;i++) {
        t1=h+S1(e)+((e&f)^(~e&g))+K256[i]+w[i];
        t2=S0(a)+((a&b)^(a&cc)^(b&cc));
        h=g;g=f;f=e;e=d+t1;d=cc;cc=b;b=a;a=t1+t2;
    }
    c->s[0]+=a;c->s[1]+=b;c->s[2]+=cc;c->s[3]+=d;c->s[4]+=e;c->s[5]+=f;c->s[6]+=g;c->s[7]+=h;
}

void sha256_init(sha256_ctx *c) {
    static const uint32_t iv[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memcpy(c->s,iv,32); c->bits=0; c->fill=0;
}
void sha256_update(sha256_ctx *c, const void *p, size_t n) {
    const uint8_t *b=(const uint8_t*)p; uint64_t total = c->bits + (uint64_t)n*8;
    if (c->fill) { size_t take = (64-c->fill) < n ? (64-c->fill) : n; memcpy(c->buf+c->fill,b,take); c->fill+=take; b+=take; n-=take;
        if (c->fill==64){ sha256_block(c,c->buf); c->fill=0; } }
    while (n>=64){ sha256_block(c,b); b+=64; n-=64; }
    if (n){ memcpy(c->buf,b,n); c->fill=n; }
    c->bits = total;
}
void sha256_final(sha256_ctx *c, uint8_t out[32]) {
    uint64_t bits=c->bits;
    uint8_t tail[72]; size_t tl=0; tail[tl++]=0x80;
    while ((c->fill+tl) % 64 != 56) tail[tl++]=0;
    for (int i=0;i<8;i++) tail[tl++]=(uint8_t)(bits>>(56-i*8));
    sha256_update(c,tail,tl);
    for (int i=0;i<8;i++){ out[i*4]=(uint8_t)(c->s[i]>>24); out[i*4+1]=(uint8_t)(c->s[i]>>16); out[i*4+2]=(uint8_t)(c->s[i]>>8); out[i*4+3]=(uint8_t)c->s[i]; }
}
void sha256(const void *p, size_t n, uint8_t out[32]) { sha256_ctx c; sha256_init(&c); sha256_update(&c,p,n); sha256_final(&c,out); }

void hmac_sha256(const uint8_t *key, size_t klen, const void *msg, size_t n, uint8_t out[32]) {
    uint8_t k[64],inner[32],pad[64];
    if (klen>64){ sha256(key,klen,k); memset(k+32,0,32);} else { memcpy(k,key,klen); memset(k+klen,0,64-klen); }
    for (int i=0;i<64;i++) pad[i]=k[i]^0x36;
    sha256_ctx c; sha256_init(&c); sha256_update(&c,pad,64); sha256_update(&c,msg,n); sha256_final(&c,inner);
    for (int i=0;i<64;i++) pad[i]=k[i]^0x5c;
    sha256_init(&c); sha256_update(&c,pad,64); sha256_update(&c,inner,32); sha256_final(&c,out);
}

void hkdf_sha256(const uint8_t *salt, size_t slen, const uint8_t *ikm, size_t ilen,
                 const char *info, size_t infolen, uint8_t *out, size_t olen) {
    uint8_t prk[32], t[32], ctr=1; size_t done=0;
    if (!slen){ uint8_t zeros[32]; memset(zeros,0,32); hmac_sha256(zeros,32,ikm,ilen,prk); }
    else hmac_sha256(salt,slen,ikm,ilen,prk);
    while (done<olen) {
        sha256_ctx c; uint8_t msg[96];
        sha256_init(&c);
        // HMAC(prk, T || info || ctr) computed via hmac_sha256 with assembled msg
        size_t off=0;
        if (done){ memcpy(msg,t,32); off=32; }
        memcpy(msg+off,info,infolen); off+=infolen;
        msg[off++]=ctr++;
        hmac_sha256(prk,32,msg,off,t);
        size_t take = olen-done<32 ? olen-done : 32;
        memcpy(out+done,t,take); done+=take;
    }
}

int rand_bytes_strong(uint8_t *p, size_t n);

void rand_bytes(uint8_t *p, size_t n) {
    if (rand_bytes_strong(p,n)==0) return;
    // last-resort fallback (degraded); secret callers use rand_bytes_strong
    // and fail closed when OS entropy is unavailable.
#ifdef _WIN32
    static uint64_t st=0x9E3779B97F4A7C15ull; static int seeded=0;
    if(!seeded){ st ^= (uint64_t)__rdtsc(); st ^= (uint64_t)(uintptr_t)p; seeded=1; }
    for (size_t i=0;i<n;i++){ st^=st<<13; st^=st>>7; st^=st<<17; p[i]=(uint8_t)(st>>56); }
#else
    uint64_t st=(uint64_t)(uintptr_t)p ^ (uint64_t)time(NULL);
    for (size_t i=0;i<n;i++){ st^=st<<13; st^=st>>7; st^=st<<17; p[i]=(uint8_t)(st>>56); }
#endif
}

int rand_bytes_strong(uint8_t *p, size_t n) {
#ifdef SYNC_HOST_TEST
    // host tests: process-unique deterministic stream; device builds are
    // unaffected and keep fail-closed semantics
    static uint64_t st=0x243F6A8885A308D3ull;
    st^=(uint64_t)(uintptr_t)p; st^=(uint64_t)n;
    for (size_t i=0;i<n;i++){ st^=st<<13; st^=st>>7; st^=st<<17; p[i]=(uint8_t)(st>>56); }
    return 0;
#endif
#ifdef _WIN32
    // ProcessPrng (advapi32) or BCryptGenRandom: real OS entropy.
    HMODULE adv = LoadLibraryA("advapi32.dll");
    if (adv) {
        typedef BOOLEAN (WINAPI *PrngFn)(PVOID,ULONG);
        PrngFn fn=(PrngFn)(void*)GetProcAddress(adv,"SystemFunction036");
        if (fn && fn(p,(ULONG)n)) { FreeLibrary(adv); return 0; }
        FreeLibrary(adv);
    }
    return -1;
#else
    size_t got=0;
    int fd=open("/dev/urandom",O_RDONLY|O_CLOEXEC);
    if (fd<0) return -1;
    while (got<n){
        ssize_t r=read(fd,p+got,n-got);
        if (r<0){ if (errno==EINTR) continue; close(fd); return -1; }
        if (r==0) { close(fd); return -1; }
        got+=(size_t)r;
    }
    close(fd);
    return 0;
#endif
}

// ---- AES-256 core ----
static const uint8_t SBOX[256]={
0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16};
static uint8_t xtime(uint8_t x){ return (uint8_t)((x<<1)^((x>>7)*0x1b)); }

typedef struct { uint32_t rk[60]; } aes_key;
static void aes_expand(aes_key *k, const uint8_t key[32]) {
    uint32_t *rk=k->rk;
    for (int i=0;i<8;i++) rk[i]=((uint32_t)key[i*4]<<24)|((uint32_t)key[i*4+1]<<16)|((uint32_t)key[i*4+2]<<8)|key[i*4+3];
    static const uint8_t rcon[7]={0x01,0x02,0x04,0x08,0x10,0x20,0x40};
    for (int i=8;i<60;i++) {
        uint32_t t=rk[i-1];
        if (i%8==0){ t=(t<<8)|(t>>24);
            t=(uint32_t)SBOX[(t>>24)&255]<<24 | (uint32_t)SBOX[(t>>16)&255]<<16 | (uint32_t)SBOX[(t>>8)&255]<<8 | (uint32_t)SBOX[t&255];
            t ^= (uint32_t)rcon[i/8-1]<<24; }
        else if (i%8==4){ t=(uint32_t)SBOX[(t>>24)&255]<<24 | (uint32_t)SBOX[(t>>16)&255]<<16 | (uint32_t)SBOX[(t>>8)&255]<<8 | (uint32_t)SBOX[t&255]; }
        rk[i]=rk[i-8]^t;
    }
}
#define PUT32(p,v) ((p)[0]=(uint8_t)((v)>>24),(p)[1]=(uint8_t)((v)>>16),(p)[2]=(uint8_t)((v)>>8),(p)[3]=(uint8_t)(v))
static void aes_ecb_block(const aes_key *k, const uint8_t in[16], uint8_t out[16]) {
    uint32_t s0,s1,s2,s3,t0,t1,t2,t3; const uint32_t *rk=k->rk;
    s0=((uint32_t)in[0]<<24)|((uint32_t)in[1]<<16)|((uint32_t)in[2]<<8)|in[3]; s0^=rk[0];
    s1=((uint32_t)in[4]<<24)|((uint32_t)in[5]<<16)|((uint32_t)in[6]<<8)|in[7]; s1^=rk[1];
    s2=((uint32_t)in[8]<<24)|((uint32_t)in[9]<<16)|((uint32_t)in[10]<<8)|in[11]; s2^=rk[2];
    s3=((uint32_t)in[12]<<24)|((uint32_t)in[13]<<16)|((uint32_t)in[14]<<8)|in[15]; s3^=rk[3];
    for (int r=1;r<14;r++) {
        t0=((uint32_t)SBOX[s0>>24]<<24)|((uint32_t)SBOX[(s1>>16)&255]<<16)|((uint32_t)SBOX[(s2>>8)&255]<<8)|(uint32_t)SBOX[s3&255];
        t1=((uint32_t)SBOX[s1>>24]<<24)|((uint32_t)SBOX[(s2>>16)&255]<<16)|((uint32_t)SBOX[(s3>>8)&255]<<8)|(uint32_t)SBOX[s0&255];
        t2=((uint32_t)SBOX[s2>>24]<<24)|((uint32_t)SBOX[(s3>>16)&255]<<16)|((uint32_t)SBOX[(s0>>8)&255]<<8)|(uint32_t)SBOX[s1&255];
        t3=((uint32_t)SBOX[s3>>24]<<24)|((uint32_t)SBOX[(s0>>16)&255]<<16)|((uint32_t)SBOX[(s1>>8)&255]<<8)|(uint32_t)SBOX[s2&255];
        const uint32_t words[4] = { t0, t1, t2, t3 };
        uint8_t o[16]; // column-major result, word w = big-endian bytes
        for (int c2=0;c2<4;c2++) {
            uint32_t w = words[c2];
            uint8_t a0=(uint8_t)(w>>24), a1=(uint8_t)(w>>16), a2=(uint8_t)(w>>8), a3=(uint8_t)w;
            uint32_t b0=(uint32_t)(xtime(a0)^(a1^xtime(a1))^a2^a3);
            uint32_t b1=(uint32_t)(a0^(xtime(a1)^a2^xtime(a2))^a3);
            uint32_t b2=(uint32_t)(a0^a1^(xtime(a2)^a3^xtime(a3)));
            uint32_t b3=(uint32_t)((xtime(a3)^(a0^xtime(a0)))^a1^a2);
            o[c2*4+0]=(uint8_t)b0; o[c2*4+1]=(uint8_t)b1; o[c2*4+2]=(uint8_t)b2; o[c2*4+3]=(uint8_t)b3;
        }
        uint32_t m0,m1,m2,m3;
        m0=((uint32_t)o[0]<<24)|((uint32_t)o[1]<<16)|((uint32_t)o[2]<<8)|o[3];
        m1=((uint32_t)o[4]<<24)|((uint32_t)o[5]<<16)|((uint32_t)o[6]<<8)|o[7];
        m2=((uint32_t)o[8]<<24)|((uint32_t)o[9]<<16)|((uint32_t)o[10]<<8)|o[11];
        m3=((uint32_t)o[12]<<24)|((uint32_t)o[13]<<16)|((uint32_t)o[14]<<8)|o[15];
        s0=m0^rk[r*4]; s1=m1^rk[r*4+1]; s2=m2^rk[r*4+2]; s3=m3^rk[r*4+3];
    }
    uint32_t f0=((uint32_t)SBOX[s0>>24]<<24)|((uint32_t)SBOX[(s1>>16)&255]<<16)|((uint32_t)SBOX[(s2>>8)&255]<<8)|(uint32_t)SBOX[s3&255];
    uint32_t f1=((uint32_t)SBOX[s1>>24]<<24)|((uint32_t)SBOX[(s2>>16)&255]<<16)|((uint32_t)SBOX[(s3>>8)&255]<<8)|(uint32_t)SBOX[s0&255];
    uint32_t f2=((uint32_t)SBOX[s2>>24]<<24)|((uint32_t)SBOX[(s3>>16)&255]<<16)|((uint32_t)SBOX[(s0>>8)&255]<<8)|(uint32_t)SBOX[s1&255];
    uint32_t f3=((uint32_t)SBOX[s3>>24]<<24)|((uint32_t)SBOX[(s0>>16)&255]<<16)|((uint32_t)SBOX[(s1>>8)&255]<<8)|(uint32_t)SBOX[s2&255];
    f0^=rk[56];f1^=rk[57];f2^=rk[58];f3^=rk[59];
    PUT32(out,f0);PUT32(out+4,f1);PUT32(out+8,f2);PUT32(out+12,f3);
}

// ---- GHASH (GF(2^128), NIST SP 800-38D: bit_0 is MSB of byte 0) ----
static void ghash_mul(uint8_t X[16], const uint8_t H[16]) {
    uint8_t Z[16]={0}, V[16];
    memcpy(V,H,16);
    for (int i=0;i<16;i++) {
        uint8_t x=X[i];
        for (int b=7;b>=0;b--) {
            if ((x>>b)&1) for (int j=0;j<16;j++) Z[j]^=V[j];
            uint8_t lsb=V[15]&1;
            for (int j=15;j>0;j--) V[j]=(uint8_t)((V[j]>>1)|(V[j-1]<<7));
            V[0]>>=1;
            if (lsb){ V[0]^=0xe1; }
        }
    }
    memcpy(X,Z,16);
}

static void ghash_ghash(const uint8_t H[16], const uint8_t *aad, size_t aad_len,
                        const uint8_t *ct, size_t n, uint8_t out[16]) {
    uint8_t X[16]={0}, lenblk[16];
    size_t total = ((aad_len+15)/16 + (n+15)/16 + 1) * 16;
    uint8_t *buf = (uint8_t*)calloc(total,1);
    size_t off=0;
    memcpy(buf, aad, aad_len); off += (aad_len+15)/16*16;
    memcpy(buf+off, ct, n); off += (n+15)/16*16;
    uint64_t abits=(uint64_t)aad_len*8, cbits=(uint64_t)n*8;
    for (int i=0;i<8;i++) lenblk[i]=(uint8_t)(abits>>(56-i*8));
    for (int i=0;i<8;i++) lenblk[8+i]=(uint8_t)(cbits>>(56-i*8));
    memcpy(buf+off,lenblk,16);
    for (size_t p=0;p<total;p+=16){ for(int j=0;j<16;j++) X[j]^=buf[p+j]; ghash_mul(X,H); }
    memcpy(out,X,16);
    free(buf);
}

int gcm_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                const uint8_t *aad, size_t aad_len,
                const uint8_t *pt, size_t n, uint8_t *ct, uint8_t tag[16]) {
    aes_key k; aes_expand(&k,key);
    uint8_t H[16]={0}, J0[16]={0}, cb[16], S[16], EKY0[16];
    aes_ecb_block(&k,H,H);
    memcpy(J0,nonce,12); J0[15]=1;
    // GCTR for plaintext using counter from J0+1
    uint8_t inc[16]; memcpy(inc,J0,16);
    for (size_t i=0;i<n;i+=16) {
        for (int j=15;j>=12;j--){ if(++inc[j]) break; }
        aes_ecb_block(&k,inc,cb);
        size_t take = n-i<16 ? n-i : 16;
        for (size_t j=0;j<take;j++) ct[i+j]=(uint8_t)(pt[i+j]^cb[j]);
    }
    ghash_ghash(H,aad,aad_len,ct,n,S);
    aes_ecb_block(&k,J0,EKY0);
    for (int j=0;j<16;j++) tag[j]=(uint8_t)(S[j]^EKY0[j]);
    return 0;
}

int gcm_decrypt(const uint8_t key[32], const uint8_t nonce[12],
                const uint8_t *aad, size_t aad_len,
                const uint8_t *ct, size_t n, uint8_t *pt, const uint8_t tag[16]) {
    aes_key k; aes_expand(&k,key);
    uint8_t H[16]={0}, J0[16]={0}, cb[16], S[16], EKY0[16], check[16];
    aes_ecb_block(&k,H,H);
    memcpy(J0,nonce,12); J0[15]=1;
    ghash_ghash(H,aad,aad_len,ct,n,S);
    aes_ecb_block(&k,J0,EKY0);
    for (int j=0;j<16;j++) check[j]=(uint8_t)(S[j]^EKY0[j]);
    uint8_t diff=0; for (int j=0;j<16;j++) diff |= (uint8_t)(check[j]^tag[j]);
    if (diff) return -1;
    uint8_t inc[16]; memcpy(inc,J0,16);
    for (size_t i=0;i<n;i+=16) {
        for (int j=15;j>=12;j--){ if(++inc[j]) break; }
        aes_ecb_block(&k,inc,cb);
        size_t take = n-i<16 ? n-i : 16;
        for (size_t j=0;j<take;j++) pt[i+j]=(uint8_t)(ct[i+j]^cb[j]);
    }
    return 0;
}
