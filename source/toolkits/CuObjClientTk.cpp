// SPDX-FileCopyrightText: 2020-2025 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "toolkits/CuObjClientTk.h"

#ifdef CUOBJ_SUPPORT

#include <exception>

#include "Logger.h"

SharedCuObjClient::SharedCuObjClient()
{
	try
	{
		// The token-based flow does not use the get/put callbacks, so empty ops
		// suffice (matches the reference SDKs' availability probe).
		client = std::make_unique<cuObjClient>(ops, CUOBJ_PROTO_RDMA_DC_V1);
		connected = client && client->isConnected();

		if(connected)
			LOGGER(Log_NORMAL, "S3 RDMA fabric connected (cuObject)." << std::endl);
		else
			LOGGER(Log_NORMAL, "S3 RDMA fabric not connected (cuObject)." << std::endl);
	}
	catch(const std::exception& e)
	{
		ERRLOGGER(Log_NORMAL, "cuObjClient init failed: " << e.what() << std::endl);
		connected = false;
	}
}

SharedCuObjClient* SharedCuObjClient::getInstance()
{
	static SharedCuObjClient instance;
	return instance.connected ? &instance : NULL;
}

bool SharedCuObjClient::registerBuffer(void* ptr, size_t size)
{
	const std::lock_guard<std::mutex> lock(mutex);

	cuObjErr_t rc = client->cuMemObjGetDescriptor(ptr, size);
	if(rc != CU_OBJ_SUCCESS)
	{
		ERRLOGGER(Log_NORMAL, "cuMemObjGetDescriptor failed. "
			"rc: " << rc << "; ptr: " << ptr << "; size: " << size << std::endl);
		return false;
	}

	return true;
}

void SharedCuObjClient::deregisterBuffer(void* ptr)
{
	const std::lock_guard<std::mutex> lock(mutex);

	if(client->cuMemObjPutDescriptor(ptr) != CU_OBJ_SUCCESS)
		ERRLOGGER(Log_VERBOSE, "cuMemObjPutDescriptor failed. ptr: " << ptr << std::endl);
}

bool SharedCuObjClient::isDeviceMemory(const void* ptr) const
{
	return cuObjClient::getMemoryType(ptr) == CUOBJ_MEMORY_CUDA_DEVICE;
}

char* SharedCuObjClient::getToken(void* ptr, size_t size, size_t offset, cuObjOpType_t op)
{
	const std::lock_guard<std::mutex> lock(mutex);

	char* token = NULL;
	cuObjErr_t rc = client->cuMemObjGetRDMAToken(ptr, size, offset, op, &token);
	if(rc != CU_OBJ_SUCCESS || !token)
	{
		ERRLOGGER(Log_NORMAL, "cuMemObjGetRDMAToken failed. "
			"rc: " << rc << "; ptr: " << ptr << "; size: " << size << "; op: " << op <<
			std::endl);
		return NULL;
	}

	return token;
}

void SharedCuObjClient::putToken(char* token)
{
	if(!token)
		return;

	const std::lock_guard<std::mutex> lock(mutex);
	client->cuMemObjPutRDMAToken(token);
}

#endif // CUOBJ_SUPPORT
