#ifndef TOOLKITS_NET_SOCKETEXCEPTION_H_
#define TOOLKITS_NET_SOCKETEXCEPTION_H_

#include "workers/WorkerException.h"


class SocketException : public WorkerException
{
	public:
		explicit SocketException(const std::string& errorMessage) : WorkerException(errorMessage) {};
};

class SocketConnectException : public SocketException
{
	public:
		explicit SocketConnectException(const std::string& errorMessage) :
			SocketException(errorMessage) {};
};

class SocketDisconnectException : public SocketException
{
	public:
		explicit SocketDisconnectException(const std::string& errorMessage) :
			SocketException(errorMessage) {};
};

class SocketTimeoutException : public SocketException
{
	public:
		explicit SocketTimeoutException(const std::string& errorMessage) :
			SocketException(errorMessage) {};
};

class SocketInterruptedPollException : public SocketException
{
	public:
		explicit SocketInterruptedPollException(const std::string& errorMessage) :
			SocketException(errorMessage) {};
};


#endif /* TOOLKITS_NET_SOCKETEXCEPTION_H_ */
