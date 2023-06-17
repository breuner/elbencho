#ifndef TOOLKITS_NET_BASICSOCKET_H_
#define TOOLKITS_NET_BASICSOCKET_H_

#include <cstring>
#include <poll.h>
#include "Common.h"
#include "Socket.h"

/**
 * Basic Linux socket, e.g. for TCP communication.
 */
class BasicSocket : public Socket
{
	public:
		BasicSocket(int domain, int type, int protocol = 0);
		virtual ~BasicSocket();

		static void createSocketPair(int domain, int type, int protocol,
				BasicSocket **outEndpointA, BasicSocket **outEndpointB);

		virtual void connect(const char *hostname, unsigned short port);
		virtual void connect(const struct sockaddr *serv_addr, socklen_t addrlen);
		virtual void bindToAddr(in_addr_t ipAddr, unsigned short port);
		virtual void listen();
		virtual Socket* accept(struct sockaddr *addr, socklen_t *addrlen);
		virtual void shutdown();
		virtual void shutdownAndRecvDisconnect(int timeoutMS);

		virtual ssize_t send(const void *buf, size_t len, int flags);
		virtual ssize_t sendto(const void *buf, size_t len, int flags, const struct sockaddr *to,
				socklen_t tolen);

		virtual ssize_t recv(void *buf, size_t len, int flags);
		virtual ssize_t recvT(void *buf, size_t len, int flags, int timeoutMS);

		ssize_t recvfrom(void *buf, size_t len, int flags, struct sockaddr *from,
				socklen_t *fromlen);
		ssize_t recvfromT(void *buf, size_t len, int flags, struct sockaddr *from,
				socklen_t *fromlen, int timeoutMS);

		ssize_t broadcast(const void *buf, size_t len, int flags, struct in_addr *broadcastIP,
				unsigned short port);

		void setSoKeepAlive(bool enable);
		void setSoBroadcast(bool enable);
		void setSoReuseAddr(bool enable);
		void setSoBindToDevice(const char* interfaceName);
		int getSoRcvBuf();
		int getSoSndBuf();
		void setSoRcvBuf(int size);
		void setSoSndBuf(int size);
		void setTcpNoDelay(bool enable);
		void setTcpCork(bool enable);

	protected:
		int sock;
		unsigned short sockDomain; // socket domain (aka protocol family) e.g. PF_INET
		const bool isDgramSocket;

		BasicSocket(int fd, unsigned short sockDomain, struct in_addr peerIP,
			std::string peername);

	private:

	// inliners
	public:

		virtual int getFD() const
			{ return sock; }

		unsigned short getSockDomain() const
			{ return sockDomain; }

		/**
		 * @throw SocketException
		 */
		ssize_t sendto(const void *buf, size_t len, int flags, struct in_addr ipAddr,
				unsigned short port)
		{
			struct sockaddr_in peerAddr;

			// memset(&peerAddr, 0, sizeof(peerAddr) ); // not required (we set all fields below)

			peerAddr.sin_family = sockDomain;
			peerAddr.sin_port = htons(port);
			peerAddr.sin_addr.s_addr = ipAddr.s_addr;

			return this->sendto(buf, len, flags, (struct sockaddr*) &peerAddr, sizeof(peerAddr));
		}

		/**
		 * @return true if incoming data is available, false if a timeout occurred
		 * @throw SocketException on error
		 */
		bool waitForIncomingData(int timeoutMS)
		{
			struct pollfd pollStruct = { sock, POLLIN, 0 };
			int pollRes = poll(&pollStruct, 1, timeoutMS);

			if((pollRes > 0) && (pollStruct.revents & POLLIN))
			{
				return true;
			}
			else
			if(!pollRes)
				return false;
			else
			if(pollStruct.revents & POLLERR)
				throw SocketException(
					std::string("waitForIncomingData(): poll(): ") + peername + ": " +
						"Error condition");
			else
			if(pollStruct.revents & POLLHUP)
				throw SocketException(
					std::string("waitForIncomingData(): poll(): ") + peername + ": Hung up");
			else
			if(pollStruct.revents & POLLNVAL)
				throw SocketException(
					std::string("waitForIncomingData(): poll(): ") + peername + ": " +
						"Invalid request/fd");
			else
			if(errno == EINTR)
				throw SocketInterruptedPollException(
					std::string("waitForIncomingData(): poll(): ") + peername + ": " +
						strerror(errno) );
			else
				throw SocketException(
					std::string("waitForIncomingData(): poll(): ") + peername + ": " +
						strerror(errno) );
		}
};


#endif /* TOOLKITS_NET_BASICSOCKET_H_ */
