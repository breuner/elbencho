// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

/*
 * Drop-in replacement for the client API of NVIDIA cuObject (cuobjclient.h of libcuobjclient).
 *
 * This header declares the same types, class and method signatures as NVIDIA's header, so that
 * a program written for cuObject compiles and links against this shim without changes. Behind
 * the API, the shim moves data over Reliable Connections on plain libibverbs instead of
 * cuObject's DC transport, for host memory only: see ../README.md. The class keeps the layout
 * of NVIDIA's (an implementation pointer plus reserved pointers), so the shim's shared library
 * can also replace libcuobjclient.so.1 at run time.
 *
 * Only the API that elbencho's S3-over-RDMA plugins use is implemented; the rest of NVIDIA's
 * header (external RC connections, telemetry details) is left out.
 */

#ifndef _CUOBJCLIENT_H_
#define _CUOBJCLIENT_H_

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <sys/types.h>

#define OBJ_RDMA_V1 "CUOBJ"
#define CUOBJ_CLIENT_MAJOR_VERSION 1
#define CUOBJ_CLIENT_MINOR_VERSION 3
#define CUOBJ_CLIENT_VERSION \
    (CUOBJ_CLIENT_MAJOR_VERSION * 1000 + CUOBJ_CLIENT_MINOR_VERSION * 10)
#define CUOBJ_MAX_MEMORY_REG_SIZE (4ULL * 1024 * 1024 * 1024 - 64 * 1024)

/* cuObject's callbacks receive the token in cuFile's RDMA info struct. Take it from cufile.h
   where the CUDA toolkit is present (then both headers can be used together), else define it. */
#if __has_include(<cufile.h>)
#include <cufile.h>
#elif !defined(__CUFILE_H_)
typedef struct cufileRDMAInfo
{
    int version;
    int desc_len;
    const char* desc_str;
} cufileRDMAInfo_t;
#endif

#ifndef CUOBJ_ERR_T_DEFINED
#define CUOBJ_ERR_T_DEFINED
typedef enum cuObjErr_enum
{
    CU_OBJ_SUCCESS = 0,
    CU_OBJ_FAIL = 1,
    CU_OBJ_RESET_FAIL = 2,
    CU_OBJ_WRONG_PROTOCOL = 3,
    CU_OBJ_EXT_RC_NOT_REGISTERED = 4,
    CU_OBJ_INVALID_VALUE = 5,
} cuObjErr_t;
#endif

#ifndef CUOBJ_PROTO_T_DEFINED
#define CUOBJ_PROTO_T_DEFINED
typedef enum cuObjProto_enum
{
    CUOBJ_PROTO_RDMA_DC_V1 = 1001,
    CUOBJ_PROTO_RDMA_EXT_RC_V1 = 1002,
    CUOBJ_PROTO_MAX
} cuObjProto_t;
#endif

#ifndef CUOBJ_OPTYPE_T_DEFINED
#define CUOBJ_OPTYPE_T_DEFINED
typedef enum cuObjOpType_enum
{
    CUOBJ_GET = 0,
    CUOBJ_PUT = 1,
    CUOBJ_INVALID = 9999
} cuObjOpType_t;
#endif

typedef struct CUObjIOOps
{
    ssize_t (*get)(const void* handle, char* ptr, size_t size, loff_t offset,
        const cufileRDMAInfo_t*);
    ssize_t (*put)(const void* handle, const char* ptr, size_t size, loff_t offset,
        const cufileRDMAInfo_t*);
} CUObjOps_t;

typedef enum cuObjMemoryType_enum
{
    CUOBJ_MEMORY_SYSTEM = 0,
    CUOBJ_MEMORY_CUDA_MANAGED = 1,
    CUOBJ_MEMORY_CUDA_DEVICE = 2,
    CUOBJ_MEMORY_UNKNOWN = 3,
    CUOBJ_MEMORY_INVALID = 4
} cuObjMemoryType_t;

class cuObjClient
{
    public:
        cuObjClient(CUObjOps_t& ops, cuObjProto_t proto = CUOBJ_PROTO_RDMA_DC_V1);
        cuObjClient(const cuObjClient& other) = delete;
        cuObjClient& operator=(const cuObjClient& other) = delete;
        ~cuObjClient();

        cuObjErr_t cuMemObjGetDescriptor(void* ptr, size_t size);
        ssize_t cuMemObjGetMaxRequestCallbackSize(void* ptr);
        cuObjErr_t cuMemObjPutDescriptor(void* ptr);
        cuObjErr_t cuMemObjGetRDMAToken(void* ptr, size_t size, size_t buffer_offset,
            cuObjOpType_t operation, char** desc_str_out);
        cuObjErr_t cuMemObjPutRDMAToken(char* desc_str);
        cuObjErr_t cuMemObjPutRDMAToken(char* desc_str, int err_code);
        static void* getCtx(const void* handle);
        ssize_t cuObjGet(void* ctx, void* ptr, size_t size, loff_t offset = 0,
            loff_t buf_offset = 0);
        ssize_t cuObjPut(void* ctx, void* ptr, size_t size, loff_t offset = 0,
            loff_t buf_offset = 0);
        bool isConnected(void);
        static void setupTelemetry(bool use_OTEL, std::ostream* os);
        static void setTelemFlags(unsigned log_flags);
        static void setTelemFlags(unsigned log_flags, unsigned log_op_flags);
        static void setOpFreq(unsigned freq_get, unsigned freq_put);
        static void shutdownTelemetry();
        static cuObjMemoryType_t getMemoryType(const void* ptr);

    private:
        struct Impl;
        Impl* _impl;
        void* _reserved[8]; // same layout as NVIDIA's class
};

#endif // _CUOBJCLIENT_H_
