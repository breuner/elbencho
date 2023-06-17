#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "BasicSocket.h"
#include "SocketException.h"


#define BASICSOCKET_CONNECT_TIMEOUT_MS	10000
#define BASICSOCKET_LISTEN_BACKLOG		128

/**
 * @throw SocketException
 */
BasicSocket::BasicSocket(int domain, int type, int protocol) :
	isDgramSocket(type == SOCK_DGRAM)
{
	this->socketType = SocketType_BASIC;

	this->sockDomain = domain;

	this->sock = ::socket(domain, type, protocol);
	if(sock == -1)
		throw SocketException(std::string("Error during socket creation: ") + strerror(errno) );
}

/**
 * Note: To be used by accept() only.
 *
 * @param fd will be closed by the destructor of this object.
 * @throw SocketException in case epoll_create fails, the caller will need to close the
 * 		corresponding socket file descriptor (fd).
 */
BasicSocket::BasicSocket(int fd, unsigned short sockDomain, struct in_addr peerIP,
		std::string peername) : isDgramSocket(false)
{
	this->sock = fd;
	this->sockDomain = sockDomain;
	this->peerIP = peerIP;
	this->peername = peername;
}

BasicSocket::~BasicSocket()
{
	close(sock);
}

/**
 * @throw SocketException
 */
void BasicSocket::createSocketPair(int domain, int type, int protocol,
		BasicSocket **outEndpointA, BasicSocket **outEndpointB)
{
	int socket_vector[2];
	struct in_addr loopbackIP = { INADDR_LOOPBACK };

	int pairRes = socketpair(domain, type, protocol, socket_vector);

	if(pairRes == -1)
	{
		throw SocketConnectException(
			std::string("Unable to create local socket pair. SysErr: ") + strerror(errno) );
	}

	*outEndpointA = NULL;
	*outEndpointB = NULL;

	try
	{
		*outEndpointA = new BasicSocket(socket_vector[0], domain, loopbackIP,
				std::string("Localhost:PeerFD#") + std::to_string(socket_vector[0]) );
		*outEndpointB = new BasicSocket(socket_vector[1], domain, loopbackIP,
				std::string("Localhost:PeerFD#") + std::to_string(socket_vector[1]) );
	} catch (SocketException &e)
	{
		if(*outEndpointA)
			delete (*outEndpointA);
		else
			close(socket_vector[0]);

		if(*outEndpointB)
			delete (*outEndpointB);
		else
			close(socket_vector[1]);

		throw;
	}
}

/**
 * @throw SocketException
 */
void BasicSocket::connect(const char *hostname, unsigned short port)
{
	Socket::connect(hostname, port, sockDomain, SOCK_STREAM);
}

/**
 * @throw SocketException
 */
void BasicSocket::connect(const struct sockaddr *serv_addr, socklen_t addrlen)
{
	unsigned short peerPort = ntohs( ( (struct sockaddr_in*) serv_addr)->sin_port);

	this->peerIP = ( (struct sockaddr_in*) serv_addr)->sin_addr;

	// set peername if not done so already (e.g. by connect(hostname) )

	if(peername.empty() )
		peername = Socket::ipaddrToStr(&peerIP) + ":" + std::to_string(peerPort);

	int connRes = ::connect(sock, serv_addr, addrlen);

	if(!connRes)
		return;

	throw SocketConnectException(
			std::string("Unable to connect to: ") + std::string(peername)
					+ std::string(". SysErr: ") + strerror(errno) );
}

/**
 * @throw SocketException
 */
void BasicSocket::bindToAddr(in_addr_t ipAddr, unsigned short port)
{
	struct sockaddr_in localaddr_in;
	memset(&localaddr_in, 0, sizeof(localaddr_in) );

	localaddr_in.sin_family = sockDomain;
	localaddr_in.sin_addr.s_addr = ipAddr;
	localaddr_in.sin_port = htons(port);

	int bindRes = ::bind(sock, (struct sockaddr*) &localaddr_in, sizeof(localaddr_in) );
	if(bindRes == -1)
		throw SocketException(
			"Unable to bind to port: " + std::to_string(port) + ". SysErr: " +
				strerror(errno) );

	peername = std::string("Listen(Port: ") + std::to_string(port) + std::string(")");
}

/**
 * @throw SocketException
 */
void BasicSocket::listen()
{
	int listenRes = ::listen(sock, BASICSOCKET_LISTEN_BACKLOG);
	if(listenRes == -1)
		throw SocketException(std::string("listen: ") + strerror(errno) );
}

/**
 * @throw SocketException
 */
Socket* BasicSocket::accept(struct sockaddr *addr, socklen_t *addrlen)
{
	int acceptRes = ::accept(sock, addr, addrlen);
	if(acceptRes == -1)
	{
		throw SocketException(std::string("Error during socket accept(): ") + strerror(errno) );
	}

	// prepare new socket object
	struct in_addr acceptIP = ( (struct sockaddr_in*) addr)->sin_addr;
	unsigned short acceptPort = ntohs( ( (struct sockaddr_in*) addr)->sin_port);

	std::string acceptPeername = endpointAddrToStr(&acceptIP, acceptPort);

	try
	{
		Socket *acceptedSock = new BasicSocket(acceptRes, sockDomain, acceptIP, acceptPeername);

		return acceptedSock;
	} catch (SocketException &e)
	{
		close(acceptRes);
		throw;
	}
}

/**
 * @throw SocketException
 */
void BasicSocket::shutdown()
{
	int shutRes = ::shutdown(sock, SHUT_WR);
	if(shutRes == -1)
	{
		throw SocketException(std::string("Error during socket shutdown(): ") + strerror(errno) );
	}
}

/**
 * @throw SocketException
 */
void BasicSocket::shutdownAndRecvDisconnect(int timeoutMS)
{
	this->shutdown();

	try
	{
		// receive until shutdown arrives
		char buf[128];
		int recvRes;
		do
		{
			recvRes = recvT(buf, sizeof(buf), 0, timeoutMS);
		} while (recvRes > 0);
	} catch (SocketDisconnectException &e)
	{
		// just a normal thing to happen when we shutdown
	}

}

/**
 * @throw SocketDisconnectException if ::send() returned -1
 */
ssize_t BasicSocket::send(const void *buf, size_t len, int flags)
{
	ssize_t sendRes = ::send(sock, buf, len, flags | MSG_NOSIGNAL);
	if(sendRes != -1)
		return sendRes;

	throw SocketDisconnectException("Disconnect during send() to: " + peername + "; "
		"SysErr: " + strerror(errno) );
}

/**
 * @throw SocketException
 */
ssize_t BasicSocket::sendto(const void *buf, size_t len, int flags, const struct sockaddr *to,
		socklen_t tolen)
{
	ssize_t sendRes = ::sendto(sock, buf, len, flags | MSG_NOSIGNAL, to, tolen);
	if(sendRes == (ssize_t) len)
		return sendRes;
	else
	if(sendRes != -1)
	{
		throw SocketException(
			std::string("send(): Sent only ") + std::to_string(sendRes) +
				std::string(" bytes of the requested ") + std::to_string(len) +
				std::string(" bytes of data") );
	}

	int errCode = errno;

	std::string toStr;

	if(to)
	{
		struct sockaddr_in *toInet = (struct sockaddr_in*) to;
		toStr = Socket::ipaddrToStr(&toInet->sin_addr) + ":"
				+ std::to_string(ntohs(toInet->sin_port) );
	}

	if(errCode == ENETUNREACH)
	{
		throw SocketException(
			"Attempted to send message to unreachable network: " + toStr + "; "
			"peername: " + peername);
	}

	throw SocketDisconnectException("sendto(" + toStr + "): "
		"Hard Disconnect from " + peername + ": " + strerror(errCode) );
}

/**
 * @throw SocketException
 */
ssize_t BasicSocket::recv(void *buf, size_t len, int flags)
{
	ssize_t recvRes = ::recv(sock, buf, len, flags);
	if(recvRes > 0)
		return recvRes;

	if(recvRes == 0)
		throw SocketDisconnectException(std::string("Soft disconnect from ") + peername);
	else
	{
		throw SocketDisconnectException(
			std::string("recv(): Hard disconnect from ") + peername + ". SysErr: " +
			strerror(errno) );
	}
}

/**
 * @throw SocketException
 */
ssize_t BasicSocket::recvT(void *buf, size_t len, int flags, int timeoutMS)
{
	ssize_t recvRes = ::recv(sock, buf, len, flags | MSG_DONTWAIT);

	if(recvRes > 0)
	{
		return recvRes;
	}
	else
	if(recvRes == 0)
	{
		throw SocketDisconnectException(std::string("recvT(): Soft Disconnect from ") + peername);
	}
	else
	if( (errno != EAGAIN) && (errno != EWOULDBLOCK) )
	{
		throw SocketDisconnectException(
			std::string("recvT(): Hard Disconnect from ") + peername + ". SysErr: " +
				strerror(errno) );
	}

	// recv would block => use poll

	do
	{
		struct pollfd pollStruct = { sock, POLLIN, 0 };
		int pollRes = poll(&pollStruct, 1, timeoutMS);

		if((pollRes > 0) && (pollStruct.revents & POLLIN))
		{
			ssize_t recvRes = ::recv(sock, buf, len, flags | MSG_DONTWAIT);

			if(recvRes > 0)
			{
				return recvRes;
			}
			else
			if(recvRes == 0)
			{
				throw SocketDisconnectException(
					std::string("recvT(): Soft Disconnect from ") + peername);
			}
			else
			if( (errno == EAGAIN) || (errno == EWOULDBLOCK) )
			{
				continue;
			}

			throw SocketDisconnectException(
				std::string("recvT(): Hard Disconnect from ") + peername + ". SysErr: " +
					strerror(errno) );
		}
		else
		if(!pollRes)
			throw SocketTimeoutException(std::string("Receive from ") + peername + " timed out");
		else
		if(pollStruct.revents & POLLERR)
			throw SocketException(
				std::string("recvT(): poll(): ") + peername + ": Error condition");
		else
		if(pollStruct.revents & POLLHUP)
			throw SocketException(std::string("recvT(): poll(): ") + peername + ": Hung up");
		else
		if(pollStruct.revents & POLLNVAL)
			throw SocketException(
				std::string("recvT(): poll(): ") + peername + ": Invalid request/fd");
		else
		if(errno == EINTR)
			throw SocketInterruptedPollException(
				std::string("recvT(): poll(): ") + peername + ": " + strerror(errno) );
		else
			throw SocketException(
				std::string("recvT(): poll(): ") + peername + ": " + strerror(errno) );

	} while (true);
}

/**
 * @throw SocketException
 */
ssize_t BasicSocket::recvfrom(void *buf, size_t len, int flags, struct sockaddr *from,
		socklen_t *fromlen)
{
	int recvRes = ::recvfrom(sock, buf, len, flags, from, fromlen);
	if(recvRes > 0)
		return recvRes;

	if(recvRes == 0)
	{
		if(isDgramSocket)
		{ // an empty datagram
			return 0;
		}
		else
			throw SocketDisconnectException(std::string("Soft disconnect from ") + peername);
	}
	else
	{
		throw SocketDisconnectException(
			std::string("recvfrom(): Hard disconnect from ") + peername + ": " +
				strerror(errno) );
	}
}

/**
 * @throw SocketException
 */
ssize_t BasicSocket::recvfromT(void *buf, size_t len, int flags, struct sockaddr *from,
		socklen_t *fromlen, int timeoutMS)
{
	struct pollfd pollStruct = { sock, POLLIN, 0 };
	int pollRes = poll(&pollStruct, 1, timeoutMS);

	if((pollRes > 0) && (pollStruct.revents & POLLIN))
	{
		int recvRes = this->recvfrom(buf, len, flags, from, fromlen);
		return recvRes;
	}
	else
	if(!pollRes)
		throw SocketTimeoutException(std::string("Receive from ") + peername + " timed out");
	else
	if(pollStruct.revents & POLLERR)
		throw SocketException(
			std::string("recvfromT(): poll(): ") + peername + ": Error condition");
	else
	if(pollStruct.revents & POLLHUP)
		throw SocketException(std::string("recvfromT(): poll(): ") + peername + ": Hung up");
	else
	if(pollStruct.revents & POLLNVAL)
		throw SocketException(
			std::string("recvfromT(): poll(): ") + peername + ": Invalid request/fd");
	else
	if(errno == EINTR)
		throw SocketInterruptedPollException(
			std::string("recvT(): poll(): ") + peername + ": " + strerror(errno) );
	else
		throw SocketException(
			std::string("recvfromT(): poll(): ") + peername + ": " + strerror(errno) );
}


/**
 * @throw SocketException
 */
ssize_t BasicSocket::broadcast(const void *buf, size_t len, int flags,
		struct in_addr *broadcastIP, unsigned short port)
{
	struct sockaddr_in broadcastAddr;

	memset(&broadcastAddr, 0, sizeof(broadcastAddr) );

	broadcastAddr.sin_family = sockDomain;
	broadcastAddr.sin_addr = *broadcastIP;
	broadcastAddr.sin_port = htons(port);

	return this->sendto(buf, len, flags, (struct sockaddr*) &broadcastAddr, sizeof(broadcastAddr) );
}

/**
 * @throw SocketException
 */
void BasicSocket::setSoKeepAlive(bool enable)
{
	int keepAliveVal = (enable ? 1 : 0);

	int setRes = setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (char*) &keepAliveVal,
		sizeof(keepAliveVal) );

	if(setRes == -1)
		throw SocketException(std::string("setSoKeepAlive: ") + strerror(errno) );
}

/**
 * @throw SocketException
 */
void BasicSocket::setSoBroadcast(bool enable)
{
	int broadcastVal = (enable ? 1 : 0);

	int setRes = setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcastVal, sizeof(broadcastVal) );

	if(setRes == -1)
		throw SocketException(std::string("setSoBroadcast: ") + strerror(errno) );
}

/**
 * @throw SocketException
 */
void BasicSocket::setSoReuseAddr(bool enable)
{
	int reuseVal = (enable ? 1 : 0);

	int setRes = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuseVal, sizeof(reuseVal) );

	if(setRes == -1)
		throw SocketException(std::string("setSoReuseAddr: ") + strerror(errno) );
}

/**
 * @param interfaceName e.g. "eth0".
 * @throw SocketException
 */
void BasicSocket::setSoBindToDevice(const char* interfaceName)
{
#ifndef SO_BINDTODEVICE
	throw SocketException("The platform where this build was created does not support "
		"SO_BINDTODEVICE for socket binding to a network device.");
#else // SO_BINDTODEVICE
	int setRes = setsockopt(
		sock, SOL_SOCKET, SO_BINDTODEVICE, interfaceName, strlen(interfaceName) );

	if(setRes == -1)
		throw SocketException(
			std::string("Binding socket to device failed: ") + strerror(errno) + "; "
			"Interface: " + interfaceName);
#endif // SO_BINDTODEVICE
}

/**
 * Get socket receive buffer size.
 *
 * @throw SocketException
 */
int BasicSocket::getSoRcvBuf()
{
	int rcvBufLen;
	socklen_t optlen = sizeof(rcvBufLen);

	int getRes = getsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvBufLen, &optlen);

	if(getRes == -1)
		throw SocketException(std::string("getSoRcvBuf: ") + strerror(errno) );

	return rcvBufLen;
}

/**
 * Get socket send buffer size.
 *
 * @throw SocketException
 */
int BasicSocket::getSoSndBuf()
{
	int rcvBufLen;
	socklen_t optlen = sizeof(rcvBufLen);

	int getRes = getsockopt(sock, SOL_SOCKET, SO_SNDBUF, &rcvBufLen, &optlen);

	if(getRes == -1)
		throw SocketException(std::string("getSoSndBuf: ") + strerror(errno) );

	return rcvBufLen;
}

/**
 * Update the max receive buf size of a socket.
 *
 * Note: Must be called prior to listen()/connect() to be effective.
 *
 * @throw SocketException
 */
void BasicSocket::setSoRcvBuf(int size)
{
	/* note: according to socket(7) man page, the value given to setsockopt() is doubled and the
		doubled value is returned by getsockopt() and by /proc entries */

	int halfSize = size / 2;

	// try without force-flag (will internally reduce to global max setting without errors)

	int setRes = setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &halfSize, sizeof(halfSize) );

	if(!setRes)
		return; //success

#ifdef SO_RCVBUFFORCE // (cygwin doesn't have this)
	// soft way did not work. try force (which only works with admin privileges).

	setRes = setsockopt(sock, SOL_SOCKET, SO_RCVBUFFORCE, &halfSize, sizeof(halfSize) );

	if(!setRes)
		return; //success
#endif // SO_RCVBUFFORCE

	if(setRes)
		throw SocketException(
			std::string("Failed to change socket receive buffer size: ") + strerror(errno) );
}

/**
 * Update the max send buf size of a socket.
 *
 * Note: Must be called prior to listen()/connect() to be effective.
 *
 * @throw SocketException
 */
void BasicSocket::setSoSndBuf(int size)
{
	/* note: according to socket(7) man page, the value given to setsockopt() is doubled and the
		doubled value is returned by getsockopt() and by /proc entries */

	int halfSize = size / 2;

	// try without force-flag (will internally reduce to global max setting without errors)

	int setRes = setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &halfSize, sizeof(halfSize) );

	if(!setRes)
		return; //success

#ifdef SO_SNDBUFFORCE // (cygwin doesn't have this)
	// soft way did not work. try force (which only works with admin privileges).

	setRes = setsockopt(sock, SOL_SOCKET, SO_SNDBUFFORCE, &halfSize, sizeof(halfSize) );

	if(!setRes)
		return; //success
#endif // SO_SNDBUFFORCE

	if(setRes)
		throw SocketException(
			std::string("Failed to change socket send buffer size: ") + strerror(errno) );
}

/**
 * @throw SocketException
 */
void BasicSocket::setTcpNoDelay(bool enable)
{
	int noDelayVal = (enable ? 1 : 0);

	int noDelayRes = setsockopt(sock,
	IPPROTO_TCP, TCP_NODELAY, (char*) &noDelayVal, sizeof(noDelayVal) );

	if(noDelayRes == -1)
		throw SocketException(std::string("setTcpNoDelay: ") + strerror(errno) );
}

/**
 * @throw SocketException
 */
void BasicSocket::setTcpCork(bool enable)
{
#ifndef TCP_CORK
	throw SocketException("The platform on which this build was created does not support TCP_CORK");
#else // TCP_CORK
	int corkVal = (enable ? 1 : 0);

	int setRes = setsockopt(sock, SOL_TCP, TCP_CORK, &corkVal, sizeof(corkVal) );

	if(setRes == -1)
		throw SocketException(std::string("setTcpCork: ") + strerror(errno) );
#endif // TCP_CORK
}


