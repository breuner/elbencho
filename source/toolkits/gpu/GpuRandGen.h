// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef TOOLKITS_GPU_GPURANDGEN_H_
#define TOOLKITS_GPU_GPURANDGEN_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "toolkits/gpu/GpuTk.h"

/* curand and hipRAND differ only in their prefix, see GpuTk.h for the same approach. */
#if defined(CUDA_SUPPORT)
    #include <curand.h>
    #define GPURAND(name)       curand##name
    #define GPURAND_CONST(name) CURAND_##name
#elif defined(HIP_SUPPORT)
    #include <hiprand/hiprand.h>
    #define GPURAND(name)       hiprand##name
    #define GPURAND_CONST(name) HIPRAND_##name
#endif

/**
 * Per-thread random number generator that fills GPU memory directly on the GPU (curand or
 * hipRAND, selected at compile time). Call init() on the GPU selected via GpuTk::setDevice().
 *
 * The generator works on its own stream, so threads sharing a GPU don't wait for each other. The
 * generate kernel runs asynchronously: a synchronous memcpy on the default stream is ordered
 * behind it, but anything that reads the GPU buffer outside of stream order (e.g. a GPU-direct
 * file write) needs sync() first.
 */
class GpuRandGen
{
    public:
        template <class EXCEPTION>
        void init(uint64_t seed)
        {
#ifdef GPU_SUPPORT
            GPURAND(Status_t) createRes = GPURAND(CreateGenerator)(&gen,
                GPURAND_CONST(RNG_PSEUDO_DEFAULT) );

            if(createRes != GPURAND_CONST(STATUS_SUCCESS) )
                throw EXCEPTION("Initialization of GPU random number generator failed. "
                    GPU_VENDOR_NAME " random generator error code: " + std::to_string(createRes) );

            GPURAND(Status_t) seedRes = GPURAND(SetPseudoRandomGeneratorSeed)(gen, seed);

            if(seedRes != GPURAND_CONST(STATUS_SUCCESS) )
                throw EXCEPTION("Seeding GPU random number generator failed. "
                    GPU_VENDOR_NAME " random generator error code: " + std::to_string(seedRes) );

            GPURT(Error_t) streamRes = GPURT(StreamCreate)(&stream);

            if(streamRes != GPURT(Success) )
                throw EXCEPTION(std::string("Creation of GPU stream for random number generator "
                    "failed. " GPU_VENDOR_NAME " Error: ") + GPURT(GetErrorString)(streamRes) );

            GPURAND(Status_t) setStreamRes = GPURAND(SetStream)(gen, stream);

            if(setStreamRes != GPURAND_CONST(STATUS_SUCCESS) )
                throw EXCEPTION("Setting stream of GPU random number generator failed. "
                    GPU_VENDOR_NAME " random generator error code: " +
                    std::to_string(setStreamRes) );
#else
            throw EXCEPTION(GPU_NOT_BUILT_MSG);
#endif
        }

        /**
         * Counterpart to init(). Safe to call multiple times or without init().
         */
        void uninit()
        {
#ifdef GPU_SUPPORT
            if(!gen)
                return;

            GPURAND(DestroyGenerator)(gen);
            gen = NULL;

            if(stream)
            {
                (void)GPURT(StreamDestroy)(stream);
                stream = NULL;
            }
#endif
        }

        /**
         * Wait until all work on the generator's stream (generate() kernels and copies queued via
         * getStream() ) is done.
         */
        template <class EXCEPTION>
        void sync()
        {
#ifdef GPU_SUPPORT
            GPURT(Error_t) res = GPURT(StreamSynchronize)(stream);

            IF_UNLIKELY(res != GPURT(Success) )
                throw EXCEPTION(std::string("Waiting for GPU random number generator failed. "
                    GPU_VENDOR_NAME " Error: ") + GPURT(GetErrorString)(res) );
#else
            throw EXCEPTION(GPU_NOT_BUILT_MSG);
#endif
        }

        /**
         * @return the generator's stream (NULL before init() or without GPU support), e.g. to
         *      queue copies that have to be ordered with the generate() kernel.
         */
        GpuStream_t getStream() const
        {
#ifdef GPU_SUPPORT
            return stream;
#else
            return NULL;
#endif
        }

        /**
         * Fill GPU memory with random 32-bit values.
         *
         * @gpuBuf GPU memory buffer, has to be 4-byte aligned.
         * @numUInts number of 32-bit values to generate.
         */
        template <class EXCEPTION>
        void generate(void* gpuBuf, size_t numUInts)
        {
#ifdef GPU_SUPPORT
            GPURAND(Status_t) res = GPURAND(Generate)(gen, (unsigned int*)gpuBuf, numUInts);

            IF_UNLIKELY(res != GPURAND_CONST(STATUS_SUCCESS) )
                throw EXCEPTION("Random number generation on GPU failed. "
                    GPU_VENDOR_NAME " random generator error code: " + std::to_string(res) );
#else
            throw EXCEPTION(GPU_NOT_BUILT_MSG);
#endif
        }

    private:
#ifdef GPU_SUPPORT
        GPURAND(Generator_t) gen{NULL};
        GpuStream_t stream{NULL};
#endif
};

#endif /* TOOLKITS_GPU_GPURANDGEN_H_ */
