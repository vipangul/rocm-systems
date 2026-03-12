// MIT License
// Copyright (c) 2025 Advanced Micro Devices, Inc. All rights reserved.
//
// Test HIP application for AQL dump tool validation.
//
// Expected AQL packet stream:
//   #0  BARRIER_AND     (hipEventRecord start)   -> GPU timestamp
//   #1  KERNEL_DISPATCH (simpleKernel iter 0)    -> raw dump + kernel descriptor
//   #2  KERNEL_DISPATCH (simpleKernel iter 1)    -> raw dump + kernel descriptor
//   ...
//   #10 KERNEL_DISPATCH (simpleKernel iter 9)    -> raw dump + kernel descriptor
//   #11 BARRIER_AND     (hipEventRecord stop)    -> GPU timestamp

#include <hip/hip_runtime.h>
#include <cstdio>
#include <cstdlib>

#define HIP_CHECK(call)                                                         \
    do                                                                          \
    {                                                                           \
        hipError_t err = (call);                                                \
        if(err != hipSuccess)                                                   \
        {                                                                       \
            fprintf(stderr, "HIP error at %s:%d — %s\n", __FILE__, __LINE__,   \
                    hipGetErrorString(err));                                     \
            exit(EXIT_FAILURE);                                                 \
        }                                                                       \
    } while(0)

__global__ void
simpleKernel(float* data, int N)
{
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if(idx < N) data[idx] = data[idx] * 2.0f;
}

int
main()
{
    const int N = 1024;
    float*    d_data;

    HIP_CHECK(hipMalloc(&d_data, N * sizeof(float)));
    HIP_CHECK(hipMemset(d_data, 1, N * sizeof(float)));

    hipEvent_t start, stop;
    HIP_CHECK(hipEventCreate(&start));
    HIP_CHECK(hipEventCreate(&stop));

    // This hipEventRecord should produce a BARRIER_AND packet (#0)
    HIP_CHECK(hipEventRecord(start));

    // These kernel launches should produce 10 KERNEL_DISPATCH packets (#1–#10)
    for(int i = 0; i < 10; i++)
    {
        simpleKernel<<<(N + 255) / 256, 256>>>(d_data, N);
    }

    // This hipEventRecord should produce a BARRIER_AND packet (#11)
    HIP_CHECK(hipEventRecord(stop));
    HIP_CHECK(hipEventSynchronize(stop));

    float ms;
    HIP_CHECK(hipEventElapsedTime(&ms, start, stop));
    printf("Elapsed: %.3f ms\n", ms);

    HIP_CHECK(hipFree(d_data));
    HIP_CHECK(hipEventDestroy(start));
    HIP_CHECK(hipEventDestroy(stop));
    return 0;
}
