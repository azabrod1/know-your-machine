// benchmark.c — measure a CPU from first principles, emit JSON.
//
// THIS IS A REFERENCE EXAMPLE written for Apple Silicon (arm64, macOS). When the
// skill runs on a different machine, the agent must PORT the platform-specific
// pieces, which are clearly marked [[PORT]] below:
//   * inline asm uses arm64 mnemonics (add/cmp/b.lo/csel/fmla, NEON .4s)  -> x86
//     needs different mnemonics/intrinsics; or fall back to portable C.
//   * sysctl reads CPU/cache info       -> Linux: /proc/cpuinfo, sysconf, /sys
//   * F_NOCACHE forces uncached disk I/O -> Linux: O_DIRECT; Windows: different
//   * 16 KB pages assumed in the TLB test -> read the real page size (4 KB x86)
//
// The METHOD is portable even when the code isn't: time a dependent op chain for
// the clock, independent chains for width, pointer-chase for memory, etc.
//
// Output: one JSON object on stdout; human progress on stderr.
// Build (reference): cc -O2 -pthread -o benchmark benchmark.c

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/statvfs.h>
#include <sys/sysctl.h>   // [[PORT]] macOS only
#include <arm_neon.h>     // [[PORT]] arm64 NEON only

static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return t.tv_sec + t.tv_nsec*1e-9; }
#define LOG(...) fprintf(stderr, __VA_ARGS__)

// ---- arm64 asm building blocks [[PORT]] ----
#define A16(x) "add " x "," x ",#1\n\t""add " x "," x ",#1\n\t""add " x "," x ",#1\n\t" \
    "add " x "," x ",#1\n\t""add " x "," x ",#1\n\t""add " x "," x ",#1\n\t""add " x "," x ",#1\n\t" \
    "add " x "," x ",#1\n\t""add " x "," x ",#1\n\t""add " x "," x ",#1\n\t""add " x "," x ",#1\n\t" \
    "add " x "," x ",#1\n\t""add " x "," x ",#1\n\t""add " x "," x ",#1\n\t""add " x "," x ",#1\n\t" \
    "add " x "," x ",#1\n\t"
static volatile uint64_t g_sink;

static double measure_ghz(void){
    uint64_t it=1u<<26,a=0; double t0=now();
    for(uint64_t i=0;i<it;i++) __asm__ volatile(A16("%w0")A16("%w0")A16("%w0")A16("%w0"):"+r"(a)::);
    return (double)it*64/(now()-t0)/1e9;          // 1 dependent add = 1 cycle
}

// ---- superscalar: 1 dependent chain vs 8 independent, blocked vs interleaved ----
#define SR8 "add %w0,%w0,#1\n\t""add %w1,%w1,#1\n\t""add %w2,%w2,#1\n\t""add %w3,%w3,#1\n\t" \
            "add %w4,%w4,#1\n\t""add %w5,%w5,#1\n\t""add %w6,%w6,#1\n\t""add %w7,%w7,#1\n\t"
static uint64_t indep8(uint64_t it){              // interleaved: 8 adjacent independent chains
    uint64_t a=0,b=0,c=0,d=0,e=0,f=0,g=0,h=0;
    for(uint64_t i=0;i<it;i++) __asm__ volatile(SR8 SR8 SR8 SR8 SR8 SR8 SR8 SR8
        :"+r"(a),"+r"(b),"+r"(c),"+r"(d),"+r"(e),"+r"(f),"+r"(g),"+r"(h));
    return a+b+c+d+e+f+g+h;
}
static uint64_t blocked8(uint64_t it){            // blocked: each chain contiguous
    uint64_t a=0,b=0,c=0,d=0,e=0,f=0,g=0,h=0;
    for(uint64_t i=0;i<it;i++) __asm__ volatile(
        A16("%w0")A16("%w0")A16("%w0")A16("%w0") A16("%w1")A16("%w1")A16("%w1")A16("%w1")
        A16("%w2")A16("%w2")A16("%w2")A16("%w2") A16("%w3")A16("%w3")A16("%w3")A16("%w3")
        A16("%w4")A16("%w4")A16("%w4")A16("%w4") A16("%w5")A16("%w5")A16("%w5")A16("%w5")
        A16("%w6")A16("%w6")A16("%w6")A16("%w6") A16("%w7")A16("%w7")A16("%w7")A16("%w7")
        :"+r"(a),"+r"(b),"+r"(c),"+r"(d),"+r"(e),"+r"(f),"+r"(g),"+r"(h));
    return a+b+c+d+e+f+g+h;
}
static double rate(uint64_t(*fn)(uint64_t), double adds_per_iter){
    uint64_t it=1u<<18;
    for(;;){ double t0=now(); volatile uint64_t s=fn(it);(void)s; if(now()-t0>0.15)break; it<<=1; }
    double best=0;
    for(int r=0;r<4;r++){ double t0=now(); volatile uint64_t s=fn(it); double dt=now()-t0;
        g_sink+=s; double v=(double)it*adds_per_iter/dt; if(v>best)best=v; }
    return best;
}

// ---- threading harness (multicore + bandwidth + flops) ----
static volatile int g_stop;
struct iarg{ uint64_t adds; };           // counter first in every arg struct
struct barg{ uint64_t bytes; uint32_t*buf; uint64_t n; };
struct farg{ uint64_t flops; };
static void* int_worker(void*p){ struct iarg*a=p; uint64_t s=0,n=0;
    while(!g_stop){ s+=indep8(4096); n+=4096ull*64; } a->adds=n; g_sink+=s; return 0; }
static void* bw_worker(void*p){ struct barg*a=p; uint32_t*b=a->buf; uint64_t k=a->n,s=0,by=0;
    while(!g_stop){ for(uint64_t i=0;i<k;i++) s+=b[i]; by+=k*4; } a->bytes=by; g_sink+=s; return 0; }
#define F16 "fmla %0.4s,%16.4s,%17.4s\n\t""fmla %1.4s,%16.4s,%17.4s\n\t""fmla %2.4s,%16.4s,%17.4s\n\t" \
 "fmla %3.4s,%16.4s,%17.4s\n\t""fmla %4.4s,%16.4s,%17.4s\n\t""fmla %5.4s,%16.4s,%17.4s\n\t" \
 "fmla %6.4s,%16.4s,%17.4s\n\t""fmla %7.4s,%16.4s,%17.4s\n\t""fmla %8.4s,%16.4s,%17.4s\n\t" \
 "fmla %9.4s,%16.4s,%17.4s\n\t""fmla %10.4s,%16.4s,%17.4s\n\t""fmla %11.4s,%16.4s,%17.4s\n\t" \
 "fmla %12.4s,%16.4s,%17.4s\n\t""fmla %13.4s,%16.4s,%17.4s\n\t""fmla %14.4s,%16.4s,%17.4s\n\t" \
 "fmla %15.4s,%16.4s,%17.4s\n\t"
static void* fma_worker(void*p){ struct farg*a=p;
    float32x4_t v0=vdupq_n_f32(0),v1=v0,v2=v0,v3=v0,v4=v0,v5=v0,v6=v0,v7=v0,
                v8=v0,v9=v0,va=v0,vb=v0,vc=v0,vd=v0,ve=v0,vf=v0,k=vdupq_n_f32(1.0000001f);
    uint64_t f=0;
    while(!g_stop){ for(int c=0;c<2000;c++) __asm__ volatile(F16
        :"+w"(v0),"+w"(v1),"+w"(v2),"+w"(v3),"+w"(v4),"+w"(v5),"+w"(v6),"+w"(v7),
         "+w"(v8),"+w"(v9),"+w"(va),"+w"(vb),"+w"(vc),"+w"(vd),"+w"(ve),"+w"(vf):"w"(k),"w"(k));
        f+=2000ull*16*4*2; }
    g_sink+=(uint64_t)vaddvq_f32(v0); a->flops=f; return 0; }
static double thread_run(void*(*fn)(void*), void*args, size_t sz, int n, double win){
    double best=0;
    for(int r=0;r<3;r++){ g_stop=0; pthread_t th[64]; double t0=now();
        for(int i=0;i<n;i++) pthread_create(&th[i],0,fn,(char*)args+i*sz);
        struct timespec ts={0,(long)(win*1e9)}; nanosleep(&ts,0); g_stop=1;
        for(int i=0;i<n;i++) pthread_join(th[i],0);
        double dt=now()-t0,tot=0; for(int i=0;i<n;i++) tot+=*(uint64_t*)((char*)args+i*sz);
        double v=tot/dt; if(v>best)best=v; }
    return best;
}

// ---- memory: pointer chase (random + sequential), Sattolo cycle ----
static uint64_t rng=88172645463325252ull;
static inline uint64_t xr(void){ rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return rng; }
#define CH16 p=A[p];p=A[p];p=A[p];p=A[p];p=A[p];p=A[p];p=A[p];p=A[p];p=A[p];p=A[p];p=A[p];p=A[p];p=A[p];p=A[p];p=A[p];p=A[p];
static volatile uint32_t g_p;
static void build_rand(uint32_t*A,uint32_t n){ for(uint32_t i=0;i<n;i++)A[i]=i;
    for(uint32_t i=n-1;i>0;i--){uint32_t j=xr()%i,t=A[i];A[i]=A[j];A[j]=t;} }
static void build_seq(uint32_t*A,uint32_t n){ for(uint32_t i=0;i<n;i++)A[i]=(i+1==n)?0:i+1; }
static double chase(uint32_t*A){
    uint64_t c=1u<<20;
    for(;;){uint32_t p=0;double t0=now();for(uint64_t i=0;i<c/16;i++){CH16}if(now()-t0>0.05){g_p=p;break;}c<<=1;}
    double best=1e9;
    for(int r=0;r<3;r++){uint32_t p=0;double t0=now();for(uint64_t i=0;i<c/16;i++){CH16}
        double per=(now()-t0)/c*1e9;g_p=p;if(per<best)best=per;}
    return best;
}

// ---- branch prediction (real branch in asm) [[PORT]] ----
static uint64_t sum_branch(const uint8_t*d,int n){ uint64_t s=0;
    #pragma clang loop unroll(disable)
    for(int i=0;i<n;i++){ uint32_t v=d[i];
        __asm__ volatile("cmp %w[v],#128\n\tb.lo 1f\n\tadd %x[s],%x[s],%w[v],uxtw\n\t1:\n\t"
            :[s]"+r"(s):[v]"r"(v):"cc"); }
    return s; }
static uint64_t sum_bless(const uint8_t*d,int n){ uint64_t s=0;
    for(int i=0;i<n;i++){ uint32_t v=d[i],t;
        __asm__ volatile("cmp %w[v],#128\n\tcsel %w[t],%w[v],wzr,hs\n\tadd %x[s],%x[s],%w[t],uxtw\n\t"
            :[s]"+r"(s),[t]"=&r"(t):[v]"r"(v):"cc"); }
    return s; }
static int cmpu8(const void*a,const void*b){return (int)*(const uint8_t*)a-(int)*(const uint8_t*)b;}
static double bcyc(uint64_t(*fn)(const uint8_t*,int),const uint8_t*d,int n,int pass,double ghz){
    double best=1e9; for(int r=0;r<5;r++){double t0=now();uint64_t s=0;
        for(int p=0;p<pass;p++)s+=fn(d,n);double dt=now()-t0;g_sink+=s;if(dt<best)best=dt;}
    return best/((double)pass*n)*1e9*ghz; }

// ---- SIMD scalar vs NEON ----
static uint64_t neon_int(uint64_t it){
    int32x4_t a=vdupq_n_s32(0),b=a,c=a,d=a,e=a,f=a,g=a,h=a,k=vdupq_n_s32(1);
    for(uint64_t i=0;i<it;i++) __asm__ volatile(
        "add %0.4s,%0.4s,%8.4s\n\tadd %1.4s,%1.4s,%8.4s\n\tadd %2.4s,%2.4s,%8.4s\n\tadd %3.4s,%3.4s,%8.4s\n\t"
        "add %4.4s,%4.4s,%8.4s\n\tadd %5.4s,%5.4s,%8.4s\n\tadd %6.4s,%6.4s,%8.4s\n\tadd %7.4s,%7.4s,%8.4s\n\t"
        :"+w"(a),"+w"(b),"+w"(c),"+w"(d),"+w"(e),"+w"(f),"+w"(g),"+w"(h):"w"(k));
    return (uint32_t)vaddvq_s32(vaddq_s32(vaddq_s32(vaddq_s32(a,b),vaddq_s32(c,d)),vaddq_s32(vaddq_s32(e,f),vaddq_s32(g,h)))); }

int main(void){
    LOG("measuring... (~30-45s)\n");
    // ---- machine info [[PORT]] ----
    char brand[256]="unknown"; size_t bl=sizeof brand;
    sysctlbyname("machdep.cpu.brand_string",brand,&bl,0,0);
    int P=0,E=0,total=0,line=0,l1d=0; int64_t mem=0,l2=0; size_t z;
    z=sizeof P; sysctlbyname("hw.perflevel0.logicalcpu",&P,&z,0,0);
    z=sizeof E; sysctlbyname("hw.perflevel1.logicalcpu",&E,&z,0,0);
    z=sizeof total; sysctlbyname("hw.logicalcpu",&total,&z,0,0);
    z=sizeof line; sysctlbyname("hw.cachelinesize",&line,&z,0,0);
    z=sizeof l1d; sysctlbyname("hw.perflevel0.l1dcachesize",&l1d,&z,0,0);
    z=sizeof l2; sysctlbyname("hw.perflevel0.l2cachesize",&l2,&z,0,0);
    z=sizeof mem; sysctlbyname("hw.memsize",&mem,&z,0,0);
    if(total==0) total=(int)sysconf(_SC_NPROCESSORS_ONLN);

    LOG("[1/9] clock + superscalar\n");
    double ghz=measure_ghz();
    double sca=rate(indep8,64), blk=rate(blocked8,512);   // indep8: 8 chains x 8 adds; blocked8: 8 x 64
    double dep1; { uint64_t it=1u<<24,a=0; double t0=now();
        for(uint64_t i=0;i<it;i++)__asm__ volatile(A16("%w0")A16("%w0")A16("%w0")A16("%w0"):"+r"(a)::);
        dep1=(double)it*64/(now()-t0); }
    double scalar_ipc=sca/(ghz*1e9), blocked_ipc=blk/(ghz*1e9);

    LOG("[2/9] multicore scaling\n");
    int tc[]={1,2,4,8,16,24,32}; double mc[7];
    struct iarg ia[64];
    for(int i=0;i<7;i++){ mc[i]=thread_run(int_worker,ia,sizeof(struct iarg),tc[i]<total?tc[i]:tc[i],0.25)/1e9; }

    LOG("[3/9] memory latency (cache hierarchy)\n");
    uint32_t *A=malloc(2200ull*1024*1024); // big buffer reused
    int kbs[]={16,64,128,256,512,1024,4096,16384,65536,262144}; double lat[10];
    for(int i=0;i<10;i++){ uint32_t n=(uint32_t)kbs[i]*256; build_rand(A,n); lat[i]=chase(A); }

    LOG("[4/9] access pattern (random vs sequential) + stream\n");
    int pk[]={64,1024,16384,65536,262144}; double rnd[5],seq[5];
    for(int i=0;i<5;i++){ uint32_t n=(uint32_t)pk[i]*256;
        build_rand(A,n); rnd[i]=chase(A); build_seq(A,n); seq[i]=chase(A); }
    double stream_gbps; { uint32_t n=64u*1024*1024; double bt=1e9;
        for(int r=0;r<3;r++){uint64_t s=0;double t0=now();for(uint32_t i=0;i<n;i++)s+=A[i];double dt=now()-t0;g_sink+=s;if(dt<bt)bt=dt;}
        stream_gbps=(double)n*4/bt/1e9; }

    LOG("[5/9] branch prediction\n");
    int bn=65536,bp=3000; uint8_t*rb=malloc(bn),*sb=malloc(bn),*ab=malloc(bn);
    uint64_t zz=12345; for(int i=0;i<bn;i++){zz^=zz<<13;zz^=zz>>7;zz^=zz<<17;rb[i]=zz&0xff;}
    for(int i=0;i<bn;i++)sb[i]=rb[i]; qsort(sb,bn,1,cmpu8);
    for(int i=0;i<bn;i++)ab[i]=(i&1)?200:50;
    double b_sorted=bcyc(sum_branch,sb,bn,bp,ghz), b_random=bcyc(sum_branch,rb,bn,bp,ghz),
           b_alt=bcyc(sum_branch,ab,bn,bp,ghz), b_less=bcyc(sum_bless,rb,bn,bp,ghz);

    LOG("[6/9] SIMD\n");
    double neon_ipc=rate(neon_int,8*4)/(ghz*1e9);
    struct farg fa1[1]; double gflops1=thread_run(fma_worker,fa1,sizeof(struct farg),1,0.25)/1e9;

    LOG("[7/9] TLB\n");
    int tl[]={64,256,1024,4096,16384,131072}; double dns[6],sps[6];
    static uint32_t order[1u<<21];
    for(int k=0;k<6;k++){ uint32_t N=tl[k];
        for(int pass2=0;pass2<2;pass2++){ uint32_t stride=pass2?4096:32;
            for(uint32_t i=0;i<N;i++)order[i]=i;
            for(uint32_t i=N-1;i>0;i--){uint32_t j=xr()%(i+1),t=order[i];order[i]=order[j];order[j]=t;}
            for(uint32_t i=0;i<N;i++)A[order[i]*stride]=order[(i+1)%N]*stride;
            double c=chase(A)*ghz; if(pass2)sps[k]=c; else dns[k]=c; } }

    LOG("[8/9] chip-wide bandwidth + flops\n");
    uint64_t bn2=16ull*1024*1024; uint32_t*big=malloc((size_t)total*bn2*4);
    for(uint64_t i=0;i<(uint64_t)total*bn2;i++)big[i]=(uint32_t)i;
    struct barg ba[64]; for(int i=0;i<total&&i<64;i++){ba[i].buf=big+i*bn2;ba[i].n=bn2;ba[i].bytes=0;}
    struct farg fa[64];
    int ct[]={1,2,4,6,8}; double bw[5],fl[5]; int nct=5;
    for(int i=0;i<nct;i++){ int n=ct[i]<=total?ct[i]:total;
        bw[i]=thread_run(bw_worker,ba,sizeof(struct barg),n,0.25)/1e9;
        fl[i]=thread_run(fma_worker,fa,sizeof(struct farg),n,0.25)/1e9; }

    LOG("[9/9] disk\n");
    double dwrite=0,drand=0; double dread[6]={0}; int dblk[]={4,16,64,256,1024,4096};
    { struct statvfs vfs; uint64_t SZ=512ull*1024*1024;
      if(statvfs(".",&vfs)==0 && (uint64_t)vfs.f_bavail*vfs.f_frsize > SZ*4){
        void*buf; posix_memalign(&buf,16384,4*1024*1024); memset(buf,0xA5,4*1024*1024);
        int fd=open("./.bench.tmp",O_RDWR|O_CREAT|O_TRUNC,0644);
        if(fd>=0){ unlink("./.bench.tmp"); fcntl(fd,F_NOCACHE,1);   // [[PORT]] F_NOCACHE
          double t0=now(); for(uint64_t o=0;o<SZ;o+=1048576)if(pwrite(fd,buf,1048576,o)<0)break; fsync(fd);
          dwrite=SZ/(now()-t0)/1e9;
          for(int i=0;i<6;i++){ size_t blk=(size_t)dblk[i]*1024; double t1=now(); uint64_t tot=0;
            for(uint64_t o=0;o<SZ;o+=blk){ssize_t r=pread(fd,buf,blk,o);if(r<=0)break;tot+=r;}
            dread[i]=tot/(now()-t1)/1e9; }
          uint64_t nb=SZ/16384,zr=0x9e37; double t2=now();
          for(int i=0;i<2000;i++){zr^=zr<<13;zr^=zr>>7;zr^=zr<<17;if(pread(fd,buf,16384,(off_t)((zr%nb)*16384))<0)break;}
          drand=(now()-t2)/2000*1e6; close(fd); }
        free(buf); } else LOG("  (skipped: low disk space)\n"); }

    // ---------- emit JSON ----------
    LOG("done. writing JSON.\n");
    printf("{\n");
    printf("  \"machine\": {\"cpu\":\"%s\",\"logical_cores\":%d,\"p_cores\":%d,\"e_cores\":%d,",brand,total,P,E);
    printf("\"ram_gb\":%.0f,\"cache_line\":%d,\"l1d_kb\":%d,\"l2_mb\":%.0f},\n",mem/1e9,line,l1d/1024,l2/1048576.0);
    printf("  \"clock_ghz\": %.3f,\n",ghz);
    printf("  \"superscalar\": {\"dependent_ipc\":%.2f,\"blocked_ipc\":%.2f,\"interleaved_ipc\":%.2f},\n",
           dep1/(ghz*1e9),blocked_ipc,scalar_ipc);
    printf("  \"multicore_gadds\": [");
    for(int i=0;i<7;i++)printf("%s{\"threads\":%d,\"gadds\":%.1f}",i?",":"",tc[i],mc[i]); printf("],\n");
    printf("  \"mem_latency\": [");
    for(int i=0;i<10;i++)printf("%s{\"kb\":%d,\"ns\":%.2f,\"cycles\":%.1f}",i?",":"",kbs[i],lat[i],lat[i]*ghz); printf("],\n");
    printf("  \"mem_pattern\": [");
    for(int i=0;i<5;i++)printf("%s{\"kb\":%d,\"random_ns\":%.2f,\"sequential_ns\":%.2f}",i?",":"",pk[i],rnd[i],seq[i]); printf("],\n");
    printf("  \"stream_gbps_1core\": %.0f,\n",stream_gbps);
    printf("  \"branch_cycles\": {\"sorted\":%.2f,\"random\":%.2f,\"alternating\":%.2f,\"branchless\":%.2f},\n",
           b_sorted,b_random,b_alt,b_less);
    printf("  \"simd\": {\"scalar_ipc\":%.1f,\"neon_int_ipc\":%.1f,\"neon_gflops_1core\":%.0f},\n",
           scalar_ipc,neon_ipc,gflops1);
    printf("  \"tlb\": [");
    for(int k=0;k<6;k++)printf("%s{\"lines\":%d,\"dense_cyc\":%.1f,\"sparse_cyc\":%.1f}",k?",":"",tl[k],dns[k],sps[k]); printf("],\n");
    printf("  \"chipwide_bandwidth\": [");
    for(int i=0;i<nct;i++)printf("%s{\"threads\":%d,\"gbps\":%.0f}",i?",":"",ct[i],bw[i]); printf("],\n");
    printf("  \"chipwide_gflops\": [");
    for(int i=0;i<nct;i++)printf("%s{\"threads\":%d,\"gflops\":%.0f}",i?",":"",ct[i],fl[i]); printf("],\n");
    printf("  \"disk\": {\"write_gbps\":%.2f,\"random_us\":%.1f,\"read_sweep\":[",dwrite,drand);
    for(int i=0;i<6;i++)printf("%s{\"block_kb\":%d,\"gbps\":%.2f}",i?",":"",dblk[i],dread[i]); printf("]}\n");
    printf("}\n");
    return 0;
}
