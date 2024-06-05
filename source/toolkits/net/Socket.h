#ifndef TOOLKITS_NET_SOCKET_H_
#define TOOLKITS_NET_SOCKET_H_

#include <netinet/in.h>
#include <sys/socket.h>
#include "Common.h"
#include "SocketException.h"

enum SocketType
{
	SocketType_UNDEFINED = 0, // initial invalid value
	SocketType_BASIC = 1, // BasicSocket
};


/**
 * Abstract base class for sockets, e.g. for TCP communication in netbench mode through the
 * derived BasicSocket class.
 */
class Socket
{
   public:
      virtual ~Socket() {}

      static std::string ipaddrToStr(struct in_addr* ipaddress);
      static std::string endpointAddrToStr(struct in_addr* ipaddress, unsigned short port);
      static std::string endpointAddrToStr(const char* hostname, unsigned short port);

      virtual void connect(const char* hostname, unsigned short port) = 0;
      virtual void connect(const struct sockaddr* serv_addr, socklen_t addrlen) = 0;
      virtual void bindToAddr(in_addr_t ipAddr, unsigned short port) = 0;
      virtual void listen() = 0;
      virtual Socket* accept(struct sockaddr* addr, socklen_t* addrlen) = 0;
      virtual void shutdown() = 0;
      virtual void shutdownAndRecvDisconnect(int timeoutMS) = 0;


      virtual ssize_t send(const void *buf, size_t len, int flags) = 0;
      virtual ssize_t sendto(const void *buf, size_t len, int flags,
         const struct sockaddr *to, socklen_t tolen) = 0;

      virtual ssize_t recv(void *buf, size_t len, int flags) = 0;
      virtual ssize_t recvT(void *buf, size_t len, int flags, int timeoutMS) = 0;

      void connect(const struct in_addr* ipaddress, unsigned short port);
      void bind(unsigned short port);


   protected:
      Socket();

      struct in_addr peerIP;
      std::string peername;
      SocketType socketType;


      void connect(const char* hostname, unsigned short port, int ai_family, int ai_socktype);


   private:


  // inliners
   public:

      uint32_t getPeerIP() const
		  { return peerIP.s_addr; }
      std::string getPeername() const
      	  { return peername; }
      SocketType getSocketType() const
      	  { return socketType; }

      /**
       * @throw SocketException
       */
      ssize_t recvExact(void *buf, size_t len, int flags)
      {
         ssize_t missing = len;

         do
         {
            ssize_t recvRes = this->recv(&((char*)buf)[len-missing], missing, flags);
            missing -= recvRes;
         } while(missing);

         return (ssize_t)len;
      }

      /**
       * @throw SocketException
       */
      ssize_t recvExactT(void *buf, size_t len, int flags, int timeoutMS)
      {
         // note: this uses a soft timeout that is being reset after each received chunk

         ssize_t missing = len;

         do
         {
            ssize_t recvRes = recvT(&((char*)buf)[len-missing], missing, flags, timeoutMS);
            missing -= recvRes;
         } while(missing);

         return (ssize_t)len;
      }

      /**
       * @throw SocketException
       */
      ssize_t recvMinMax(void *buf, size_t minLen, size_t maxLen, int flags)
      {
         size_t receivedLen = 0;

         do
         {
            ssize_t recvRes = this->recv(&((char*)buf)[receivedLen], maxLen-receivedLen, flags);
            receivedLen += recvRes;
         } while(receivedLen < minLen);

         return (ssize_t)receivedLen;
      }

      /**
       * @throw SocketException
       */
      ssize_t recvMinMaxT(void *buf, ssize_t minLen, ssize_t maxLen, int flags,
         int timeoutMS)
      {
         // note: this uses a soft timeout that is being reset after each received chunk

         ssize_t receivedLen = 0;

         do
         {
            ssize_t recvRes =
               recvT(&((char*)buf)[receivedLen], maxLen-receivedLen, flags, timeoutMS);
            receivedLen += recvRes;
         } while(receivedLen < minLen);

         return (ssize_t)receivedLen;
      }

};


#endif /* TOOLKITS_NET_SOCKET_H_ */
