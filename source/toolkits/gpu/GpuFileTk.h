// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef TOOLKITS_GPU_GPUFILETK_H_
#define TOOLKITS_GPU_GPUFILETK_H_

#include <cstddef>
#include <string>
#include <sys/types.h>

#include "toolkits/gpu/GpuFileHandleData.h"

/**
 * Vendor-neutral wrappers for the GPU-direct file API (cuFile or hipFile, selected at compile
 * time), see GpuFileHandleData.h for the vendor selection. read() and write() are the plain vendor
 * calls for the I/O path; the other wrappers are for phase setup and teardown.
 */
class GpuFileTk
{
    public:
        // operation names for the ops log
        static constexpr const char* READ_OP_NAME = GPUFILE_API_NAME "Read";
        static constexpr const char* WRITE_OP_NAME = GPUFILE_API_NAME "Write";

        /**
         * Explicitly initialize the library and open its driver. Optional, the first registration
         * does this implicitly otherwise. Counterpart is driverClose().
         */
        template <class EXCEPTION>
        static void driverOpen()
        {
#ifdef GPUFILE_SUPPORT
            GpuFileError_t res = GPUFILE(DriverOpen)();

            if(res.err != GPUFILE_SUCCESS)
                throw EXCEPTION(std::string(GPUFILE_API_NAME " driver open failed. ") +
                    GPUFILE_API_NAME " Error: " + GPUFILE_ERRSTR(res.err) );
#else
            throw EXCEPTION(GPUFILE_NOT_BUILT_MSG);
#endif
        }

        /**
         * Counterpart to driverOpen(). Safe to call without GPU-direct support (no-op then).
         */
        static void driverClose()
        {
#ifdef GPUFILE_SUPPORT
            (void)GPUFILE(DriverClose)();
#endif
        }

        /**
         * Register a GPU memory buffer for DMA. Counterpart is bufDeregister().
         *
         * @gpuID only for the error message.
         */
        template <class EXCEPTION>
        static void bufRegister(const void* gpuBuf, size_t len, int gpuID)
        {
#ifdef GPUFILE_SUPPORT
            GpuFileError_t res = GPUFILE(BufRegister)(gpuBuf, len, 0);

            if(res.err != GPUFILE_SUCCESS)
                throw EXCEPTION(std::string("GPU DMA buffer registration failed. ") +
                    "GPU ID: " + std::to_string(gpuID) + "; "
                    GPUFILE_API_NAME " Error: " + GPUFILE_ERRSTR(res.err) );
#else
            throw EXCEPTION(GPUFILE_NOT_BUILT_MSG);
#endif
        }

        /**
         * Counterpart to bufRegister(). Safe to call without GPU-direct support (no-op then).
         *
         * @return NULL on success, otherwise the API's error string.
         */
        static const char* bufDeregister(const void* gpuBuf)
        {
#ifdef GPUFILE_SUPPORT
            GpuFileError_t res = GPUFILE(BufDeregister)(gpuBuf);

            if(res.err != GPUFILE_SUCCESS)
                return GPUFILE_ERRSTR(res.err);
#endif
            return NULL;
        }

        /**
         * Positional sync read from a registered file into GPU memory.
         *
         * @return number of bytes read, -1 on system error (see errno), other negative values
         *      are API error codes.
         */
        static ssize_t read(const GpuFileHandleData& handleData, void* gpuBuf, size_t nbytes,
            off_t offset)
        {
#ifdef GPUFILE_SUPPORT
            return GPUFILE(Read)(handleData.handle, gpuBuf, nbytes, offset, 0);
#else
            return -1;
#endif
        }

        /**
         * Positional sync write from GPU memory to a registered file.
         *
         * @return number of bytes written, -1 on system error (see errno), other negative values
         *      are API error codes.
         */
        static ssize_t write(const GpuFileHandleData& handleData, const void* gpuBuf,
            size_t nbytes, off_t offset)
        {
#ifdef GPUFILE_SUPPORT
            return GPUFILE(Write)(handleData.handle, gpuBuf, nbytes, offset, 0);
#else
            return -1;
#endif
        }
};

#endif /* TOOLKITS_GPU_GPUFILETK_H_ */
