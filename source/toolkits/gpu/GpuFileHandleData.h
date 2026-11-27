// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef TOOLKITS_GPU_GPUFILEHANDLEDATA_H_
#define TOOLKITS_GPU_GPUFILEHANDLEDATA_H_

#include <cstring>
#include <string>
#include <vector>

#include "Common.h"

#if defined(CUFILE_SUPPORT) && defined(HIPFILE_SUPPORT)
    #error "CUFILE_SUPPORT and HIPFILE_SUPPORT are mutually exclusive (one GPU vendor per executable)"
#endif

#if (defined(CUFILE_SUPPORT) || defined(HIPFILE_SUPPORT) ) && !defined(GPUFILE_SUPPORT)
    #error "GPUFILE_SUPPORT has to be defined together with CUFILE_SUPPORT or HIPFILE_SUPPORT"
#endif

/* cuFile (NVIDIA GPUDirect Storage) and hipFile (AMD) are the same API with a different prefix,
   so the functions are selected by the prefix macro and only the few differently named types and
   constants get a vendor-neutral alias here. Used by GpuFileHandleData and GpuFileTk. */
#if defined(CUFILE_SUPPORT)
    #include <cufile.h>
    #define GPUFILE_API_NAME        "cuFile"
    #define GPUFILE(name)           cuFile##name
    #define GPUFILE_SUCCESS         CU_FILE_SUCCESS
    #define GPUFILE_HANDLE_TYPE_FD  CU_FILE_HANDLE_TYPE_OPAQUE_FD
    #define GPUFILE_ERRSTR(err)     CUFILE_ERRSTR(err)
    typedef CUfileError_t  GpuFileError_t;
    typedef CUfileDescr_t  GpuFileDescr_t;
    typedef CUfileHandle_t GpuFileHandle_t;
#elif defined(HIPFILE_SUPPORT)
    #include <hipfile.h>
    #define GPUFILE_API_NAME        "hipFile"
    #define GPUFILE(name)           hipFile##name
    #define GPUFILE_SUCCESS         hipFileSuccess
    #define GPUFILE_HANDLE_TYPE_FD  hipFileHandleTypeOpaqueFD
    #define GPUFILE_ERRSTR(err)     HIPFILE_ERRSTR(err)
    typedef hipFileError_t  GpuFileError_t;
    typedef hipFileDescr_t  GpuFileDescr_t;
    typedef hipFileHandle_t GpuFileHandle_t;
#else
    #define GPUFILE_API_NAME        "cuFile/hipFile"
#endif

#define GPUFILE_NOT_BUILT_MSG   "GPU-direct file API requested, but this executable was built " \
                                "without cuFile/hipFile support."


/**
 * Per-file handle for the GPU-direct file API (cuFile or hipFile).
 */
struct GpuFileHandleData
{
    #ifdef GPUFILE_SUPPORT
        GpuFileDescr_t descr;
        GpuFileHandle_t handle;
        int fd{-1}; // hint for dereg: !=-1 if handle registration was successful
    #endif

    /**
     * Register the file for GPU-direct reads/writes. Call deregisterHandle() when done with the
     * file access.
     *
     * @fd posix file descriptor to register.
     * @throw template EXCEPTION if registration fails (e.g. WorkerException or ProgException).
     */
    template <class EXCEPTION>
    void registerHandle(int fd)
    {
        #ifdef GPUFILE_SUPPORT

            memset( (void*)&descr, 0, sizeof(descr) );

            descr.handle.fd = fd;
            descr.type = GPUFILE_HANDLE_TYPE_FD;

            GpuFileError_t registerRes = GPUFILE(HandleRegister)(&handle, &descr);

            if(registerRes.err != GPUFILE_SUCCESS)
                throw EXCEPTION(std::string(GPUFILE_API_NAME " file handle registration failed. ")
                    + GPUFILE_API_NAME " Error: " + GPUFILE_ERRSTR(registerRes.err) );

            this->fd = fd;

        #else
            throw EXCEPTION(GPUFILE_NOT_BUILT_MSG);
        #endif // GPUFILE_SUPPORT
    }

    /**
     * Counterpart to registerHandle(). This is safe to call multiple times or even if
     * registration was not called or if it failed, based on "this->fd == -1".
     */
    void deregisterHandle()
    {
        #ifdef GPUFILE_SUPPORT

            IF_UNLIKELY(fd == -1)
                return;

            GPUFILE(HandleDeregister)(handle);

            fd = -1;

        #endif // GPUFILE_SUPPORT
    }
};

typedef std::vector<GpuFileHandleData> GpuFileHandleDataVec;
typedef std::vector<GpuFileHandleData*> GpuFileHandleDataPtrVec;


#endif /* TOOLKITS_GPU_GPUFILEHANDLEDATA_H_ */
