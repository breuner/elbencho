// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef TOOLKITS_GPU_GPUTK_H_
#define TOOLKITS_GPU_GPUTK_H_

#include <cstddef>
#include <string>

#include "Common.h"

#if defined(CUDA_SUPPORT) && defined(HIP_SUPPORT)
    #error "CUDA_SUPPORT and HIP_SUPPORT are mutually exclusive (one GPU vendor per executable)"
#endif

#if (defined(CUDA_SUPPORT) || defined(HIP_SUPPORT) ) && !defined(GPU_SUPPORT)
    #error "GPU_SUPPORT has to be defined together with CUDA_SUPPORT or HIP_SUPPORT"
#endif

/* The CUDA and HIP runtime APIs differ only in their prefix (cudaMalloc/hipMalloc, ...), so the
   vendor is selected by this prefix macro and every wrapper below has a single body. */
#if defined(CUDA_SUPPORT)
    #include <cuda_runtime.h>
    #define GPU_VENDOR_NAME     "CUDA"
    #define GPURT(name)         cuda##name
#elif defined(HIP_SUPPORT)
    #include <hip/hip_runtime_api.h>
    #define GPU_VENDOR_NAME     "HIP"
    #define GPURT(name)         hip##name
#else
    #define GPU_VENDOR_NAME     "none"
#endif

#define GPU_NOT_BUILT_MSG   "GPU requested, but this executable was built without GPU support " \
                            "(CUDA/HIP)."

#ifdef GPU_SUPPORT
typedef GPURT(Stream_t) GpuStream_t;
#else
typedef void* GpuStream_t; // placeholder, every GPU function throws in builds without GPU support
#endif

/**
 * Vendor-neutral wrappers for the GPU runtime (CUDA or HIP, selected at compile time). Each
 * wrapper is the plain vendor call plus the result check; the error message is only built on
 * failure, so these are safe for the I/O path. Without GPU support, every wrapper that needs a
 * GPU throws the given EXCEPTION type.
 */
class GpuTk
{
    public:
        /**
         * @return number of GPUs in the system.
         * @throw EXCEPTION if the GPU runtime reports an error.
         */
        template <class EXCEPTION>
        static int getDeviceCount()
        {
#ifdef GPU_SUPPORT
            int numDevices;

            GPURT(Error_t) res = GPURT(GetDeviceCount)(&numDevices);

            if(res != GPURT(Success) )
                throw EXCEPTION(std::string("Getting GPU device count failed. ") +
                    GPU_VENDOR_NAME " Error: " + GPURT(GetErrorString)(res) );

            return numDevices;
#else
            throw EXCEPTION(GPU_NOT_BUILT_MSG);
#endif
        }

        /**
         * Select the GPU for all following GPU calls of the calling thread.
         */
        template <class EXCEPTION>
        static void setDevice(int gpuID)
        {
#ifdef GPU_SUPPORT
            GPURT(Error_t) res = GPURT(SetDevice)(gpuID);

            if(res != GPURT(Success) )
                throw EXCEPTION("Setting GPU device failed. "
                    "GPU ID: " + std::to_string(gpuID) + "; "
                    GPU_VENDOR_NAME " Error: " + GPURT(GetErrorString)(res) );
#else
            throw EXCEPTION(GPU_NOT_BUILT_MSG);
#endif
        }

        /**
         * Allocate GPU memory on the GPU selected via setDevice().
         *
         * @gpuID only for the error message.
         * @return the GPU memory buffer, to be freed via free().
         */
        template <class EXCEPTION>
        static void* malloc(size_t len, int gpuID)
        {
#ifdef GPU_SUPPORT
            void* gpuBuf;

            GPURT(Error_t) res = GPURT(Malloc)(&gpuBuf, len);

            if(res != GPURT(Success) )
                throw EXCEPTION("GPU memory allocation failed. "
                    "Buffer size: " + std::to_string(len) + "; "
                    "GPU ID: " + std::to_string(gpuID) + "; "
                    GPU_VENDOR_NAME " Error: " + GPURT(GetErrorString)(res) );

            return gpuBuf;
#else
            throw EXCEPTION(GPU_NOT_BUILT_MSG);
#endif
        }

        /**
         * Free a buffer from malloc(). Safe to call without GPU support (no-op then).
         */
        static void free(void* gpuBuf)
        {
#ifdef GPU_SUPPORT
            (void)GPURT(Free)(gpuBuf);
#endif
        }

        /**
         * Pin a host memory buffer and register it with the GPU runtime for faster transfers.
         */
        template <class EXCEPTION>
        static void hostRegister(void* hostBuf, size_t len)
        {
#ifdef GPU_SUPPORT
            GPURT(Error_t) res = GPURT(HostRegister)(hostBuf, len, GPURT(HostRegisterDefault) );

            if(res != GPURT(Success) )
                throw EXCEPTION("Registration of host buffer with the GPU runtime failed. "
                    "Buffer size: " + std::to_string(len) + "; "
                    GPU_VENDOR_NAME " Error: " + GPURT(GetErrorString)(res) );
#else
            throw EXCEPTION(GPU_NOT_BUILT_MSG);
#endif
        }

        /**
         * Counterpart to hostRegister(). Safe to call without GPU support (no-op then).
         *
         * @return NULL on success, otherwise the GPU runtime's error string.
         */
        static const char* hostUnregister(void* hostBuf)
        {
#ifdef GPU_SUPPORT
            GPURT(Error_t) res = GPURT(HostUnregister)(hostBuf);

            if(res != GPURT(Success) )
                return GPURT(GetErrorString)(res);
#endif
            return NULL;
        }

        /**
         * Copy from host memory to GPU memory (e.g. to simulate the transfer of file contents to
         * the GPU after a file read).
         *
         * @gpuID only for the error message.
         */
        template <class EXCEPTION>
        static void memcpyHostToGpu(void* gpuBuf, const void* hostBuf, size_t count, int gpuID)
        {
#ifdef GPU_SUPPORT
            GPURT(Error_t) res = GPURT(Memcpy)(gpuBuf, hostBuf, count, GPURT(MemcpyHostToDevice) );

            IF_UNLIKELY(res != GPURT(Success) )
                throw EXCEPTION("Copy from host buffer to GPU buffer failed. "
                    "Byte count: " + std::to_string(count) + "; "
                    "GPU ID: " + std::to_string(gpuID) + "; "
                    GPU_VENDOR_NAME " Error: " + GPURT(GetErrorString)(res) );
#else
            throw EXCEPTION(GPU_NOT_BUILT_MSG);
#endif
        }

        /**
         * Asynchronous copy from host memory to GPU memory, ordered within the given stream.
         * Returns before the copy is done, so synchronize the stream before using the GPU buffer
         * outside of that stream (e.g. for a GPU-direct file write).
         *
         * @gpuID only for the error message.
         */
        template <class EXCEPTION>
        static void memcpyHostToGpuAsync(void* gpuBuf, const void* hostBuf, size_t count,
            GpuStream_t stream, int gpuID)
        {
#ifdef GPU_SUPPORT
            GPURT(Error_t) res = GPURT(MemcpyAsync)(gpuBuf, hostBuf, count,
                GPURT(MemcpyHostToDevice), stream);

            IF_UNLIKELY(res != GPURT(Success) )
                throw EXCEPTION("Async copy from host buffer to GPU buffer failed. "
                    "Byte count: " + std::to_string(count) + "; "
                    "GPU ID: " + std::to_string(gpuID) + "; "
                    GPU_VENDOR_NAME " Error: " + GPURT(GetErrorString)(res) );
#else
            throw EXCEPTION(GPU_NOT_BUILT_MSG);
#endif
        }

        /**
         * Copy from GPU memory to host memory (e.g. to simulate the transfer of file contents from
         * the GPU before a file write).
         *
         * @gpuID only for the error message.
         */
        template <class EXCEPTION>
        static void memcpyGpuToHost(void* hostBuf, const void* gpuBuf, size_t count, int gpuID)
        {
#ifdef GPU_SUPPORT
            GPURT(Error_t) res = GPURT(Memcpy)(hostBuf, gpuBuf, count, GPURT(MemcpyDeviceToHost) );

            IF_UNLIKELY(res != GPURT(Success) )
                throw EXCEPTION("Copy from GPU buffer to host buffer failed. "
                    "Byte count: " + std::to_string(count) + "; "
                    "GPU ID: " + std::to_string(gpuID) + "; "
                    GPU_VENDOR_NAME " Error: " + GPURT(GetErrorString)(res) );
#else
            throw EXCEPTION(GPU_NOT_BUILT_MSG);
#endif
        }
};

#endif /* TOOLKITS_GPU_GPUTK_H_ */
