// gpubench.m — put a stopwatch on the GPU (the thousands-wide machine).
//
// Tests, mirroring the CPU course:
//   1. FP32 FMA throughput  -> the GPU's real TFLOP/s (vs the CPU's 1.9)
//   2. occupancy sweep      -> throughput vs #threads: a GPU needs MASSES of
//                              threads to be busy (it hides latency with
//                              parallelism, not out-of-order tricks)
//   3. memory bandwidth     -> streaming read GB/s (GPU taps more of the shared
//                              unified-memory bandwidth than the CPU can)
//   4. FP16 (half) FMA      -> Apple GPUs run half precision ~2x (the ML number)
//   5. matrix multiply      -> a REAL workload, naive vs tiled: locality on the
//                              GPU, the same lesson as random-vs-sequential
//
// Build: clang -fobjc-arc -O2 -framework Metal -framework Foundation gpubench.m
//
// Anti-cheat: loop trip counts (`iters`/`N`) and the multiplier (`coef`) are
// kernel ARGUMENTS, unknown to the shader compiler, so it can't fold them away.

#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <stdio.h>

static const char *SHADER =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"kernel void fma_bench(device float* out [[buffer(0)]],\n"
"                      constant uint& iters [[buffer(1)]],\n"
"                      constant float& coef [[buffer(2)]],\n"
"                      uint gid [[thread_position_in_grid]]) {\n"
"  float c=coef;\n"
"  float a0=gid*1e-7f,a1=a0+1,a2=a0+2,a3=a0+3,a4=a0+4,a5=a0+5,a6=a0+6,a7=a0+7;\n"
"  for (uint i=0;i<iters;i++){\n"
"    a0=fma(a0,c,c);a1=fma(a1,c,c);a2=fma(a2,c,c);a3=fma(a3,c,c);\n"
"    a4=fma(a4,c,c);a5=fma(a5,c,c);a6=fma(a6,c,c);a7=fma(a7,c,c);\n"
"  }\n"
"  out[gid]=a0+a1+a2+a3+a4+a5+a6+a7;\n"
"}\n"
"kernel void fma_half(device half* out [[buffer(0)]],\n"
"                     constant uint& iters [[buffer(1)]],\n"
"                     constant half& coef [[buffer(2)]],\n"
"                     uint gid [[thread_position_in_grid]]) {\n"
"  half c=coef;\n"
"  half a0=gid*1e-4h,a1=a0+1,a2=a0+2,a3=a0+3,a4=a0+4,a5=a0+5,a6=a0+6,a7=a0+7;\n"
"  for (uint i=0;i<iters;i++){\n"
"    a0=fma(a0,c,c);a1=fma(a1,c,c);a2=fma(a2,c,c);a3=fma(a3,c,c);\n"
"    a4=fma(a4,c,c);a5=fma(a5,c,c);a6=fma(a6,c,c);a7=fma(a7,c,c);\n"
"  }\n"
"  out[gid]=a0+a1+a2+a3+a4+a5+a6+a7;\n"
"}\n"
"kernel void bw_read(device const float* in [[buffer(0)]],\n"
"                    constant uint& n [[buffer(1)]],\n"
"                    device float* out [[buffer(2)]],\n"
"                    uint gid [[thread_position_in_grid]],\n"
"                    uint gsize [[threads_per_grid]]) {\n"
"  float s=0; for (uint i=gid;i<n;i+=gsize) s+=in[i]; out[gid]=s;\n"
"}\n"
"kernel void matmul_naive(device const float* A [[buffer(0)]],\n"
"                         device const float* B [[buffer(1)]],\n"
"                         device float* C [[buffer(2)]],\n"
"                         constant uint& N [[buffer(3)]],\n"
"                         uint2 gid [[thread_position_in_grid]]) {\n"
"  uint row=gid.y,col=gid.x; if(row>=N||col>=N) return;\n"
"  float s=0; for(uint k=0;k<N;k++) s+=A[row*N+k]*B[k*N+col];\n"
"  C[row*N+col]=s;\n"
"}\n"
"kernel void matmul_tiled(device const float* A [[buffer(0)]],\n"
"                         device const float* B [[buffer(1)]],\n"
"                         device float* C [[buffer(2)]],\n"
"                         constant uint& N [[buffer(3)]],\n"
"                         uint2 lid [[thread_position_in_threadgroup]],\n"
"                         uint2 tg  [[threadgroup_position_in_grid]]) {\n"
"  threadgroup float As[16][16]; threadgroup float Bs[16][16];\n"
"  uint row=tg.y*16+lid.y, col=tg.x*16+lid.x; float s=0;\n"
"  for(uint t=0;t<N/16;t++){\n"
"    As[lid.y][lid.x]=A[row*N + t*16 + lid.x];\n"
"    Bs[lid.y][lid.x]=B[(t*16+lid.y)*N + col];\n"
"    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
"    for(uint k=0;k<16;k++) s+=As[lid.y][k]*Bs[k][lid.x];\n"
"    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
"  }\n"
"  C[row*N+col]=s;\n"
"}\n";

static double run_kernel(id<MTLCommandQueue> q, id<MTLComputePipelineState> pipe,
                         void(^setup)(id<MTLComputeCommandEncoder>), NSUInteger gridN){
    id<MTLCommandBuffer> cmd=[q commandBuffer];
    id<MTLComputeCommandEncoder> enc=[cmd computeCommandEncoder];
    [enc setComputePipelineState:pipe]; setup(enc);
    NSUInteger tg=pipe.maxTotalThreadsPerThreadgroup; if(tg>256) tg=256;
    [enc dispatchThreads:MTLSizeMake(gridN,1,1) threadsPerThreadgroup:MTLSizeMake(tg,1,1)];
    [enc endEncoding]; [cmd commit]; [cmd waitUntilCompleted];
    return cmd.GPUEndTime - cmd.GPUStartTime;
}
static double best_time(id<MTLCommandQueue> q, id<MTLComputePipelineState> pipe,
                        void(^setup)(id<MTLComputeCommandEncoder>), NSUInteger gridN, int reps){
    double best=1e30;
    for(int r=0;r<reps;r++){ double t=run_kernel(q,pipe,setup,gridN); if(t<best)best=t; }
    return best;
}
static double run_matmul(id<MTLCommandQueue> q, id<MTLComputePipelineState> pipe,
                         id<MTLBuffer> A,id<MTLBuffer> B,id<MTLBuffer> C, uint32_t N, BOOL tiled){
    double best=1e30;
    for(int r=0;r<3;r++){
        id<MTLCommandBuffer> cmd=[q commandBuffer];
        id<MTLComputeCommandEncoder> enc=[cmd computeCommandEncoder];
        [enc setComputePipelineState:pipe];
        [enc setBuffer:A offset:0 atIndex:0]; [enc setBuffer:B offset:0 atIndex:1];
        [enc setBuffer:C offset:0 atIndex:2]; [enc setBytes:&N length:sizeof(N) atIndex:3];
        if(tiled) [enc dispatchThreadgroups:MTLSizeMake(N/16,N/16,1)
                            threadsPerThreadgroup:MTLSizeMake(16,16,1)];
        else      [enc dispatchThreads:MTLSizeMake(N,N,1)
                     threadsPerThreadgroup:MTLSizeMake(16,16,1)];
        [enc endEncoding]; [cmd commit]; [cmd waitUntilCompleted];
        double t=cmd.GPUEndTime-cmd.GPUStartTime; if(t<best)best=t;
    }
    return best;
}

int main(void){
  @autoreleasepool {
    id<MTLDevice> dev=MTLCreateSystemDefaultDevice();
    if(!dev){ printf("no Metal device\n"); return 1; }
    printf("GPU: %s  (sees %.0f GB of unified memory)\n\n",
           dev.name.UTF8String, dev.recommendedMaxWorkingSetSize/1e9);
    NSError *err=nil;
    id<MTLLibrary> lib=[dev newLibraryWithSource:[NSString stringWithUTF8String:SHADER]
                                          options:nil error:&err];
    if(!lib){ printf("compile failed: %s\n", err.description.UTF8String); return 1; }
    id<MTLCommandQueue> q=[dev newCommandQueue];
    #define PIPE(name) [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@name] error:&err]
    id<MTLComputePipelineState> fma=PIPE("fma_bench"), fmaH=PIPE("fma_half"),
        bw=PIPE("bw_read"), mmN=PIPE("matmul_naive"), mmT=PIPE("matmul_tiled");

    NSUInteger maxGrid=1u<<22; uint32_t iters=4096; float coef=1.0000001f;
    id<MTLBuffer> outBuf=[dev newBufferWithLength:maxGrid*sizeof(float) options:MTLResourceStorageModeShared];

    // 1. FP32 FMA peak
    void(^fmaSetup)(id<MTLComputeCommandEncoder>)=^(id<MTLComputeCommandEncoder> enc){
        [enc setBuffer:outBuf offset:0 atIndex:0];
        [enc setBytes:&iters length:4 atIndex:1]; [enc setBytes:&coef length:4 atIndex:2]; };
    double t=best_time(q,fma,fmaSetup,maxGrid,5);
    double flops=(double)maxGrid*iters*8*2;
    printf("1. FP32 FMA peak : %6.2f TFLOP/s\n", flops/t/1e12);

    // 2. occupancy sweep
    printf("\n2. occupancy — throughput vs #GPU threads:\n     threads     GFLOP/s\n");
    NSUInteger grids[]={1u<<10,1u<<12,1u<<14,1u<<16,1u<<18,1u<<20,1u<<22};
    for(int i=0;i<7;i++){ double ti=best_time(q,fma,fmaSetup,grids[i],5);
        printf("    %8lu    %8.0f\n",(unsigned long)grids[i],(double)grids[i]*iters*16/ti/1e9); }

    // 3. bandwidth
    uint32_t n=256u*1024*1024;
    id<MTLBuffer> inBuf=[dev newBufferWithLength:(NSUInteger)n*4 options:MTLResourceStorageModeShared];
    float *p=(float*)inBuf.contents; for(uint32_t i=0;i<n;i++) p[i]=(float)i;
    double tb=best_time(q,bw,^(id<MTLComputeCommandEncoder> enc){
        [enc setBuffer:inBuf offset:0 atIndex:0]; [enc setBytes:&n length:4 atIndex:1];
        [enc setBuffer:outBuf offset:0 atIndex:2]; },1u<<20,5);
    printf("\n3. memory bandwidth (read 1 GB) : %.0f GB/s\n", (double)n*4/tb/1e9);

    // 4. FP16 half FMA
    id<MTLBuffer> hOut=[dev newBufferWithLength:maxGrid*2 options:MTLResourceStorageModeShared];
    __fp16 hcoef=0.5;
    double th=best_time(q,fmaH,^(id<MTLComputeCommandEncoder> enc){
        [enc setBuffer:hOut offset:0 atIndex:0];
        [enc setBytes:&iters length:4 atIndex:1]; [enc setBytes:&hcoef length:2 atIndex:2]; },maxGrid,5);
    printf("\n4. FP16 (half) FMA peak : %6.2f TFLOP/s  (%.1fx the FP32 rate)\n",
           flops/th/1e12, t/th);

    // 5. matrix multiply (N=2048), naive vs tiled
    uint32_t N=2048; NSUInteger msz=(NSUInteger)N*N*4;
    id<MTLBuffer> A=[dev newBufferWithLength:msz options:MTLResourceStorageModeShared];
    id<MTLBuffer> B=[dev newBufferWithLength:msz options:MTLResourceStorageModeShared];
    id<MTLBuffer> C=[dev newBufferWithLength:msz options:MTLResourceStorageModeShared];
    float *pa=(float*)A.contents,*pb=(float*)B.contents;
    for(uint32_t i=0;i<N*N;i++){ pa[i]=(float)((i%13)+1)*0.1f; pb[i]=(float)((i%7)+1)*0.1f; }
    double mflops=2.0*N*N*N;
    double tn=run_matmul(q,mmN,A,B,C,N,NO);
    double tt=run_matmul(q,mmT,A,B,C,N,YES);
    printf("\n5. matrix multiply (2048x2048):\n");
    printf("     naive  : %6.0f GFLOP/s\n", mflops/tn/1e9);
    printf("     tiled  : %6.0f GFLOP/s   (%.1fx faster — same math, better locality)\n",
           mflops/tt/1e9, tn/tt);
    printf("     (C[0]=%.1f — confirms it actually ran)\n", ((float*)C.contents)[0]);
    return 0;
  }
}
