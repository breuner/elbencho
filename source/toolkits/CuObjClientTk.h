// SPDX-FileCopyrightText: 2020-2025 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef TOOLKITS_CUOBJCLIENTTK_H_
#define TOOLKITS_CUOBJCLIENTTK_H_

#ifdef CUOBJ_SUPPORT

#include <cstddef>
#include <memory>
#include <mutex>

#include <cuobjclient.h>

/**
 * Process-wide cuObjClient singleton (NVIDIA cuObject, CUDA 13.1+).
 *
 * A single cuObjClient instance per process is the supported usage pattern, so
 * buffer registration and token minting are serialized through an internal mutex.
 * This wraps the token-based flow: the data payload moves out-of-band over RDMA
 * while the S3 control plane relays the minted token via the x-amz-rdma-* headers
 * (see S3RdmaTk).
 */
class SharedCuObjClient
{
	public:
		/**
		 * Returns the process-wide instance, or NULL if the RDMA fabric is
		 * unavailable (cuObjClient could not connect).
		 */
		static SharedCuObjClient* getInstance();

		bool isConnected() const { return connected; }

		/**
		 * Pin a buffer for RDMA. Required before minting a token for it.
		 * @return true on success.
		 */
		bool registerBuffer(void* ptr, size_t size);

		/**
		 * Release a buffer registration acquired via registerBuffer().
		 */
		void deregisterBuffer(void* ptr);

		/**
		 * @return true if the pointer is CUDA device (VRAM) memory.
		 */
		bool isDeviceMemory(const void* ptr) const;

		/**
		 * Mint an RDMA token for a registered buffer. The caller must release the
		 * returned token via putToken().
		 * @return token string on success, NULL on failure.
		 */
		char* getToken(void* ptr, size_t size, size_t offset, cuObjOpType_t op);

		/**
		 * Release an RDMA token acquired via getToken().
		 */
		void putToken(char* token);

	private:
		SharedCuObjClient();

		CUObjIOOps ops{}; // empty callbacks: the token-based flow does not use them
		std::unique_ptr<cuObjClient> client;
		bool connected{false};
		std::mutex mutex;
};

#endif // CUOBJ_SUPPORT

#endif // TOOLKITS_CUOBJCLIENTTK_H_
