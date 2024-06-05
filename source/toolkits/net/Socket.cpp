#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include "Socket.h"

/**
 * @throw SocketException (possibly by derived classes)
 */
Socket::Socket()
{
	peerIP.s_addr = 0;
	socketType = SocketType_UNDEFINED;
}

/**
 * @throw SocketException
 */
void Socket::connect(const char *hostname, unsigned short port, int ai_family, int ai_socktype)
{
	struct addrinfo hint;
	struct addrinfo *addressList;

	memset(&hint, 0, sizeof(struct addrinfo));
	hint.ai_flags = AI_CANONNAME;
	hint.ai_family = ai_family;
	hint.ai_socktype = ai_socktype;

	int getRes = getaddrinfo(hostname, NULL, &hint, &addressList);
	if(getRes)
		throw SocketConnectException(
			std::string("Unable to resolve hostname: ") + std::string(hostname) );

	// set port and peername
	((struct sockaddr_in*) addressList->ai_addr)->sin_port = htons(port);

	peername = (strlen(addressList->ai_canonname) ? addressList->ai_canonname : hostname);
	peername += std::string(":") + std::to_string(port);

	try
	{
		connect(addressList->ai_addr, addressList->ai_addrlen);

		freeaddrinfo(addressList);
	} catch (...)
	{
		freeaddrinfo(addressList);
		throw;
	}

}

/**
 * Note: Sets the protocol family type to AF_INET.
 *
 * @throw SocketException
 */
void Socket::connect(const struct in_addr *ipaddress, unsigned short port)
{
	struct sockaddr_in serv_addr;

	memset(&serv_addr, 0, sizeof(serv_addr));

	serv_addr.sin_family = AF_INET; //sockDomain;
	serv_addr.sin_addr.s_addr = ipaddress->s_addr;
	serv_addr.sin_port = htons(port);

	this->connect( (struct sockaddr*) &serv_addr, sizeof(serv_addr) );
}

/**
 * @throw SocketException
 */
void Socket::bind(unsigned short port)
{
	in_addr_t ipAddr = INADDR_ANY;

	this->bindToAddr(ipAddr, port);
}

std::string Socket::ipaddrToStr(struct in_addr *ipaddress)
{
	unsigned char *ipBytes = (unsigned char*) &ipaddress->s_addr;

	std::string ipString = std::to_string(ipBytes[0]) + "." + std::to_string(ipBytes[1]) + "." +
		std::to_string(ipBytes[2]) + "." + std::to_string(ipBytes[3]);

	return ipString;
}

std::string Socket::endpointAddrToStr(struct in_addr *ipaddress, unsigned short port)
{
	return Socket::ipaddrToStr(ipaddress) + ":" + std::to_string(port);
}

std::string Socket::endpointAddrToStr(const char *hostname, unsigned short port)
{
	return std::string(hostname) + ":" + std::to_string(port);
}

