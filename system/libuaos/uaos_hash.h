/* uaos_hash.h — freestanding hash implementations for UAOS gnu: layer
 *
 * Compact, self-contained implementations of MD5, SHA-1, SHA-256, SHA-512,
 * CRC32, and BLAKE2b for use by the *sum coreutils.  No standard library
 * required.  Each algorithm exposes an init/update/final interface.
 */

#ifndef UAOS_HASH_H
#define UAOS_HASH_H

#include <stdint.h>
#include <stddef.h>
#include "uaos_libc.h"

/* =========================================================================
 * CRC32 (for cksum — uses the POSIX/CRC-32 polynomial 0x04C11DB7)
 * ========================================================================= */
static uint32_t g_crc32_table[256];
static int g_crc32_init = 0;

static void crc32_init_table(void)
{
    if (g_crc32_init) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i << 24;
        for (int j = 0; j < 8; j++) {
            if (c & 0x80000000) c = (c << 1) ^ 0x04C11DB7;
            else c <<= 1;
        }
        g_crc32_table[i] = c;
    }
    g_crc32_init = 1;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    crc32_init_table();
    for (size_t i = 0; i < len; i++)
        crc = (crc << 8) ^ g_crc32_table[((crc >> 24) ^ data[i]) & 0xFF];
    return crc;
}

/* =========================================================================
 * MD5
 * ========================================================================= */
typedef struct {
    uint32_t state[4];
    uint64_t count;
    uint8_t  buffer[64];
} md5_ctx;

static const uint32_t md5_k[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,
    0xa8304613,0xfd469501,0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,
    0x6b901122,0xfd987193,0xa679438e,0x49b40821,0xf61e2562,0xc040b340,
    0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,
    0x676f02d9,0x8d2a4c8a,0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,
    0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,0x289b7ec6,0xeaa127fa,
    0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,
    0xffeff47d,0x85845dd1,0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,
    0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
};

static const int md5_s[64] = {
    7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
    5, 9,14,20,5, 9,14,20,5, 9,14,20,5, 9,14,20,
    4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
    6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21
};

#define MD5_ROL(x,n) (((x)<<(n))|((x)>>(32-(n))))

static void md5_transform(uint32_t st[4], const uint8_t block[64])
{
    uint32_t a=st[0],b=st[1],c=st[2],d=st[3];
    uint32_t M[16];
    for (int i=0;i<16;i++)
        M[i]=(uint32_t)block[i*4]|((uint32_t)block[i*4+1]<<8)|((uint32_t)block[i*4+2]<<16)|((uint32_t)block[i*4+3]<<24);
    for (int i=0;i<64;i++){
        uint32_t f; int g;
        if(i<16){f=(b&c)|(~b&d);g=i;}
        else if(i<32){f=(d&b)|(~d&c);g=(5*i+1)%16;}
        else if(i<48){f=b^c^d;g=(3*i+5)%16;}
        else{f=c^(b|~d);g=(7*i)%16;}
        uint32_t temp=d; d=c; c=b;
        b=b+MD5_ROL(a+f+md5_k[i]+M[g],md5_s[i]);
        a=temp;
    }
    st[0]+=a; st[1]+=b; st[2]+=c; st[3]+=d;
}

static void md5_init(md5_ctx *ctx)
{
    ctx->state[0]=0x67452301; ctx->state[1]=0xefcdab89;
    ctx->state[2]=0x98badcfe; ctx->state[3]=0x10325476;
    ctx->count=0;
}

static void md5_update(md5_ctx *ctx, const uint8_t *data, size_t len)
{
    size_t idx=(size_t)(ctx->count&63);
    ctx->count+=len;
    if(idx){
        size_t need=64-idx;
        if(len<need){ uaos_memcpy(ctx->buffer+idx,data,len); return; }
        uaos_memcpy(ctx->buffer+idx,data,need);
        md5_transform(ctx->state,ctx->buffer);
        data+=need; len-=need;
    }
    while(len>=64){ md5_transform(ctx->state,data); data+=64; len-=64; }
    if(len) uaos_memcpy(ctx->buffer,data,len);
}

static void md5_final(md5_ctx *ctx, uint8_t out[16])
{
    uint64_t bits=ctx->count*8;
    size_t idx=(size_t)(ctx->count&63);
    ctx->buffer[idx++]=0x80;
    if(idx>56){
        uaos_memset(ctx->buffer+idx,0,64-idx);
        md5_transform(ctx->state,ctx->buffer);
        idx=0;
    }
    uaos_memset(ctx->buffer+idx,0,56-idx);
    for(int i=0;i<8;i++) ctx->buffer[56+i]=(uint8_t)(bits>>(i*8));
    md5_transform(ctx->state,ctx->buffer);
    for(int i=0;i<4;i++){
        out[i*4]  =(uint8_t)(ctx->state[i]);
        out[i*4+1]=(uint8_t)(ctx->state[i]>>8);
        out[i*4+2]=(uint8_t)(ctx->state[i]>>16);
        out[i*4+3]=(uint8_t)(ctx->state[i]>>24);
    }
}

/* =========================================================================
 * SHA-1
 * ========================================================================= */
typedef struct {
    uint32_t state[5];
    uint64_t count;
    uint8_t  buffer[64];
} sha1_ctx;

static void sha1_transform(uint32_t st[5], const uint8_t block[64])
{
    uint32_t w[80];
    for(int i=0;i<16;i++)
        w[i]=((uint32_t)block[i*4]<<24)|((uint32_t)block[i*4+1]<<16)|((uint32_t)block[i*4+2]<<8)|block[i*4+3];
    for(int i=16;i<80;i++)
        w[i]=((w[i-3]^w[i-8]^w[i-14]^w[i-16])<<1)|((w[i-3]^w[i-8]^w[i-14]^w[i-16])>>31);
    uint32_t a=st[0],b=st[1],c=st[2],d=st[3],e=st[4];
    for(int i=0;i<80;i++){
        uint32_t f,k;
        if(i<20){f=(b&c)|(~b&d);k=0x5A827999;}
        else if(i<40){f=b^c^d;k=0x6ED9EBA1;}
        else if(i<60){f=(b&c)|(b&d)|(c&d);k=0x8F1BBCDC;}
        else{f=b^c^d;k=0xCA62C1D6;}
        uint32_t temp=((a<<5)|(a>>27))+f+e+k+w[i];
        e=d; d=c; c=(b<<30)|(b>>2); b=a; a=temp;
    }
    st[0]+=a; st[1]+=b; st[2]+=c; st[3]+=d; st[4]+=e;
}

static void sha1_init(sha1_ctx *ctx)
{
    ctx->state[0]=0x67452301; ctx->state[1]=0xEFCDAB89;
    ctx->state[2]=0x98BADCFE; ctx->state[3]=0x10325476;
    ctx->state[4]=0xC3D2E1F0;
    ctx->count=0;
}

static void sha1_update(sha1_ctx *ctx, const uint8_t *data, size_t len)
{
    size_t idx=(size_t)(ctx->count&63);
    ctx->count+=len;
    if(idx){
        size_t need=64-idx;
        if(len<need){uaos_memcpy(ctx->buffer+idx,data,len);return;}
        uaos_memcpy(ctx->buffer+idx,data,need);
        sha1_transform(ctx->state,ctx->buffer);
        data+=need; len-=need;
    }
    while(len>=64){sha1_transform(ctx->state,data);data+=64;len-=64;}
    if(len)uaos_memcpy(ctx->buffer,data,len);
}

static void sha1_final(sha1_ctx *ctx, uint8_t out[20])
{
    uint64_t bits=ctx->count*8;
    size_t idx=(size_t)(ctx->count&63);
    ctx->buffer[idx++]=0x80;
    if(idx>56){
        uaos_memset(ctx->buffer+idx,0,64-idx);
        sha1_transform(ctx->state,ctx->buffer);
        idx=0;
    }
    uaos_memset(ctx->buffer+idx,0,56-idx);
    for(int i=0;i<8;i++) ctx->buffer[56+i]=(uint8_t)(bits>>(56-i*8));
    sha1_transform(ctx->state,ctx->buffer);
    for(int i=0;i<5;i++){
        out[i*4]  =(uint8_t)(ctx->state[i]>>24);
        out[i*4+1]=(uint8_t)(ctx->state[i]>>16);
        out[i*4+2]=(uint8_t)(ctx->state[i]>>8);
        out[i*4+3]=(uint8_t)(ctx->state[i]);
    }
}

/* =========================================================================
 * SHA-256
 * ========================================================================= */
typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t  buffer[64];
} sha256_ctx;

static const uint32_t sha256_k[64]={
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,
    0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,
    0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
    0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,
    0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,
    0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define SHA256_ROR(x,n) (((x)>>(n))|((x)<<(32-(n))))

static void sha256_transform(uint32_t st[8], const uint8_t block[64])
{
    uint32_t w[64];
    for(int i=0;i<16;i++)
        w[i]=((uint32_t)block[i*4]<<24)|((uint32_t)block[i*4+1]<<16)|((uint32_t)block[i*4+2]<<8)|block[i*4+3];
    for(int i=16;i<64;i++){
        uint32_t s0=SHA256_ROR(w[i-15],7)^SHA256_ROR(w[i-15],18)^(w[i-15]>>3);
        uint32_t s1=SHA256_ROR(w[i-2],17)^SHA256_ROR(w[i-2],19)^(w[i-2]>>10);
        w[i]=w[i-16]+s0+w[i-7]+s1;
    }
    uint32_t a=st[0],b=st[1],c=st[2],d=st[3],e=st[4],f=st[5],g=st[6],h=st[7];
    for(int i=0;i<64;i++){
        uint32_t S1=SHA256_ROR(e,6)^SHA256_ROR(e,11)^SHA256_ROR(e,25);
        uint32_t ch=(e&f)^(~e&g);
        uint32_t t1=h+S1+ch+sha256_k[i]+w[i];
        uint32_t S0=SHA256_ROR(a,2)^SHA256_ROR(a,13)^SHA256_ROR(a,22);
        uint32_t maj=(a&b)^(a&c)^(b&c);
        uint32_t t2=S0+maj;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    st[0]+=a;st[1]+=b;st[2]+=c;st[3]+=d;st[4]+=e;st[5]+=f;st[6]+=g;st[7]+=h;
}

static void sha256_init(sha256_ctx *ctx)
{
    ctx->state[0]=0x6a09e667;ctx->state[1]=0xbb67ae85;
    ctx->state[2]=0x3c6ef372;ctx->state[3]=0xa54ff53a;
    ctx->state[4]=0x510e527f;ctx->state[5]=0x9b05688c;
    ctx->state[6]=0x1f83d9ab;ctx->state[7]=0x5be0cd19;
    ctx->count=0;
}

static void sha256_update(sha256_ctx *ctx, const uint8_t *data, size_t len)
{
    size_t idx=(size_t)(ctx->count&63);
    ctx->count+=len;
    if(idx){
        size_t need=64-idx;
        if(len<need){uaos_memcpy(ctx->buffer+idx,data,len);return;}
        uaos_memcpy(ctx->buffer+idx,data,need);
        sha256_transform(ctx->state,ctx->buffer);
        data+=need;len-=need;
    }
    while(len>=64){sha256_transform(ctx->state,data);data+=64;len-=64;}
    if(len)uaos_memcpy(ctx->buffer,data,len);
}

static void sha256_final(sha256_ctx *ctx, uint8_t out[32])
{
    uint64_t bits=ctx->count*8;
    size_t idx=(size_t)(ctx->count&63);
    ctx->buffer[idx++]=0x80;
    if(idx>56){
        uaos_memset(ctx->buffer+idx,0,64-idx);
        sha256_transform(ctx->state,ctx->buffer);
        idx=0;
    }
    uaos_memset(ctx->buffer+idx,0,56-idx);
    for(int i=0;i<8;i++) ctx->buffer[56+i]=(uint8_t)(bits>>(56-i*8));
    sha256_transform(ctx->state,ctx->buffer);
    for(int i=0;i<8;i++){
        out[i*4]  =(uint8_t)(ctx->state[i]>>24);
        out[i*4+1]=(uint8_t)(ctx->state[i]>>16);
        out[i*4+2]=(uint8_t)(ctx->state[i]>>8);
        out[i*4+3]=(uint8_t)(ctx->state[i]);
    }
}

/* =========================================================================
 * SHA-512
 * ========================================================================= */
typedef struct {
    uint64_t state[8];
    uint64_t count_lo, count_hi;
    uint8_t  buffer[128];
} sha512_ctx;

static const uint64_t sha512_k[80]={
    0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,
    0xe9b5dba58189dbbcULL,0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,
    0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,0xd807aa98a3030242ULL,
    0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,
    0xc19bf174cf692694ULL,0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,
    0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,0x2de92c6f592b0275ULL,
    0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,
    0xbf597fc7beef0ee4ULL,0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL,0x142929670a0e6e70ULL,0x27b70a8546d22ffcULL,
    0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
    0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,
    0x92722c851482353bULL,0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,
    0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,0xd192e819d6ef5218ULL,
    0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,
    0x34b0bcb5e19b48a8ULL,0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,
    0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,0x748f82ee5defb2fcULL,
    0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,
    0xc67178f2e372532bULL,0xca273eceea26619cULL,0xd186b8c721c0c207ULL,
    0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,0x06f067aa72176fbaULL,
    0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
    0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,
    0x431d67c49c100d4cULL,0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,
    0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL
};

#define SHA512_ROR(x,n) (((x)>>(n))|((x)<<(64-(n))))

static void sha512_transform(uint64_t st[8], const uint8_t block[128])
{
    uint64_t w[80];
    for(int i=0;i<16;i++){
        w[i]=0;
        for(int j=0;j<8;j++) w[i]=(w[i]<<8)|block[i*8+j];
    }
    for(int i=16;i<80;i++){
        uint64_t s0=SHA512_ROR(w[i-15],1)^SHA512_ROR(w[i-15],8)^(w[i-15]>>7);
        uint64_t s1=SHA512_ROR(w[i-2],19)^SHA512_ROR(w[i-2],61)^(w[i-2]>>6);
        w[i]=w[i-16]+s0+w[i-7]+s1;
    }
    uint64_t a=st[0],b=st[1],c=st[2],d=st[3],e=st[4],f=st[5],g=st[6],h=st[7];
    for(int i=0;i<80;i++){
        uint64_t S1=SHA512_ROR(e,14)^SHA512_ROR(e,18)^SHA512_ROR(e,41);
        uint64_t ch=(e&f)^(~e&g);
        uint64_t t1=h+S1+ch+sha512_k[i]+w[i];
        uint64_t S0=SHA512_ROR(a,28)^SHA512_ROR(a,34)^SHA512_ROR(a,39);
        uint64_t maj=(a&b)^(a&c)^(b&c);
        uint64_t t2=S0+maj;
        h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    st[0]+=a;st[1]+=b;st[2]+=c;st[3]+=d;st[4]+=e;st[5]+=f;st[6]+=g;st[7]+=h;
}

static void sha512_init(sha512_ctx *ctx)
{
    ctx->state[0]=0x6a09e667f3bcc908ULL;ctx->state[1]=0xbb67ae8584caa73bULL;
    ctx->state[2]=0x3c6ef372fe94f82bULL;ctx->state[3]=0xa54ff53a5f1d36f1ULL;
    ctx->state[4]=0x510e527fade682d1ULL;ctx->state[5]=0x9b05688c2b3e6c1fULL;
    ctx->state[6]=0x1f83d9abfb41bd6bULL;ctx->state[7]=0x5be0cd19137e2179ULL;
    ctx->count_lo=0;ctx->count_hi=0;
}

static void sha512_update(sha512_ctx *ctx, const uint8_t *data, size_t len)
{
    size_t idx=(size_t)(ctx->count_lo&127);
    ctx->count_lo+=(uint64_t)len;
    if(ctx->count_lo<(uint64_t)len) ctx->count_hi++;
    if(idx){
        size_t need=128-idx;
        if(len<need){uaos_memcpy(ctx->buffer+idx,data,len);return;}
        uaos_memcpy(ctx->buffer+idx,data,need);
        sha512_transform(ctx->state,ctx->buffer);
        data+=need;len-=need;
    }
    while(len>=128){sha512_transform(ctx->state,data);data+=128;len-=128;}
    if(len)uaos_memcpy(ctx->buffer,data,len);
}

static void sha512_final(sha512_ctx *ctx, uint8_t out[64])
{
    uint64_t bits_lo=ctx->count_lo*8;
    uint64_t bits_hi=ctx->count_hi*8+((ctx->count_lo>>61)&7);
    size_t idx=(size_t)(ctx->count_lo&127);
    ctx->buffer[idx++]=0x80;
    if(idx>112){
        uaos_memset(ctx->buffer+idx,0,128-idx);
        sha512_transform(ctx->state,ctx->buffer);
        idx=0;
    }
    uaos_memset(ctx->buffer+idx,0,112-idx);
    for(int i=0;i<8;i++) ctx->buffer[112+i]=(uint8_t)(bits_hi>>(56-i*8));
    for(int i=0;i<8;i++) ctx->buffer[120+i]=(uint8_t)(bits_lo>>(56-i*8));
    sha512_transform(ctx->state,ctx->buffer);
    for(int i=0;i<8;i++)
        for(int j=0;j<8;j++) out[i*8+j]=(uint8_t)(ctx->state[i]>>(56-j*8));
}

/* =========================================================================
 * BLAKE2b
 * ========================================================================= */
typedef struct {
    uint64_t h[8];
    uint64_t t[2];
    uint64_t f[2];
    uint8_t  buffer[128];
    size_t   buflen;
} blake2b_ctx;

static const uint64_t blake2b_iv[8]={
    0x6a09e667f3bcc908ULL,0xbb67ae8584caa73bULL,0x3c6ef372fe94f82bULL,
    0xa54ff53a5f1d36f1ULL,0x510e527fade682d1ULL,0x9b05688c2b3e6c1fULL,
    0x1f83d9abfb41bd6bULL,0x5be0cd19137e2179ULL
};

static const uint8_t blake2b_sigma[12][16]={
    {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15},
    {14,10,4,8,9,15,13,6,1,12,0,2,11,7,5,3},
    {11,8,12,0,5,2,15,13,10,14,3,6,7,1,9,4},
    {7,9,3,1,13,12,11,14,2,6,5,10,4,0,15,8},
    {9,0,5,7,2,4,10,15,14,1,11,12,3,6,8,13},
    {2,12,6,10,0,11,8,3,4,13,7,5,15,14,1,9},
    {12,5,1,15,14,13,4,10,0,7,6,3,9,2,8,11},
    {13,11,7,14,12,1,3,9,5,0,15,4,8,6,2,10},
    {6,15,14,9,11,3,0,8,12,2,13,7,1,4,10,5},
    {10,2,8,4,7,6,1,5,15,11,9,14,3,12,13,0},
    {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15},
    {14,10,4,8,9,15,13,6,1,12,0,2,11,7,5,3}
};

#define B2B_G(a,b,c,d,x,y) do{ \
    v[a]+=v[b]+x; v[d]=SHA512_ROR(v[d]^v[a],32); \
    v[c]+=v[d]; v[b]=SHA512_ROR(v[b]^v[c],24); \
    v[a]+=v[b]+y; v[d]=SHA512_ROR(v[d]^v[a],16); \
    v[c]+=v[d]; v[b]=SHA512_ROR(v[b]^v[c],63); \
}while(0)

static void blake2b_compress(blake2b_ctx *ctx, const uint8_t block[128])
{
    uint64_t v[16];
    uint64_t m[16];
    for(int i=0;i<8;i++) v[i]=ctx->h[i];
    for(int i=0;i<8;i++) v[8+i]=blake2b_iv[i];
    v[12]^=ctx->t[0];
    v[13]^=ctx->t[1];
    v[14]^=ctx->f[0];
    v[15]^=ctx->f[1];
    for(int i=0;i<16;i++){
        m[i]=0;
        for(int j=0;j<8;j++) m[i]=(m[i]<<8)|block[i*8+j];
    }
    for(int r=0;r<12;r++){
        const uint8_t *s=blake2b_sigma[r];
        B2B_G(0,4,8,12,m[s[0]],m[s[1]]);
        B2B_G(1,5,9,13,m[s[2]],m[s[3]]);
        B2B_G(2,6,10,14,m[s[4]],m[s[5]]);
        B2B_G(3,7,11,15,m[s[6]],m[s[7]]);
        B2B_G(0,5,10,15,m[s[8]],m[s[9]]);
        B2B_G(1,6,11,12,m[s[10]],m[s[11]]);
        B2B_G(2,7,8,13,m[s[12]],m[s[13]]);
        B2B_G(3,4,9,14,m[s[14]],m[s[15]]);
    }
    for(int i=0;i<8;i++) ctx->h[i]^=v[i]^v[i+8];
}

static void blake2b_init(blake2b_ctx *ctx)
{
    for(int i=0;i<8;i++) ctx->h[i]=blake2b_iv[i];
    ctx->h[0]^=0x01010000ULL; /* key length=0, digest length=64 */
    ctx->t[0]=ctx->t[1]=0;
    ctx->f[0]=ctx->f[1]=0;
    ctx->buflen=0;
}

static void blake2b_update(blake2b_ctx *ctx, const uint8_t *data, size_t len)
{
    while(len>0){
        size_t need=128-ctx->buflen;
        if(len>need){
            uaos_memcpy(ctx->buffer+ctx->buflen,data,need);
            ctx->t[0]+=(uint64_t)128;
            if(ctx->t[0]<128) ctx->t[1]++;
            blake2b_compress(ctx,ctx->buffer);
            ctx->buflen=0;
            data+=need; len-=need;
        } else {
            uaos_memcpy(ctx->buffer+ctx->buflen,data,len);
            ctx->buflen+=len;
            len=0;
        }
    }
}

static void blake2b_final(blake2b_ctx *ctx, uint8_t out[64])
{
    ctx->t[0]+=(uint64_t)ctx->buflen;
    if(ctx->t[0]<ctx->buflen) ctx->t[1]++;
    ctx->f[0]=~(uint64_t)0;
    uaos_memset(ctx->buffer+ctx->buflen,0,128-ctx->buflen);
    blake2b_compress(ctx,ctx->buffer);
    for(int i=0;i<8;i++)
        for(int j=0;j<8;j++) out[i*8+j]=(uint8_t)(ctx->h[i]>>(j*8));
}

/* =========================================================================
 * Hex output helper
 * ========================================================================= */
static void hash_to_hex(const uint8_t *hash, int len, char *out)
{
    static const char *hexchars = "0123456789abcdef";
    for (int i = 0; i < len; i++) {
        out[i * 2]     = hexchars[hash[i] >> 4];
        out[i * 2 + 1] = hexchars[hash[i] & 0xF];
    }
    out[len * 2] = '\0';
}

#endif /* UAOS_HASH_H */
