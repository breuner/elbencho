#ifndef WORKERS_WORKEREXCEPTION_H_
#define WORKERS_WORKEREXCEPTION_H_

#include "ProgException.h"

/**
 * For errors with explanation message in worker threads.
 *
 * WorkerException in main (coordinator) thread are only for errors which have already been noticed
 * and logged by workers themselves, so we main thread won't print any error messages from these
 * exceptions.
 */
class WorkerException : public ProgException
{
	public:
		explicit WorkerException(const std::string& errorMessage) : ProgException(errorMessage) {};
};


/**
 * For use by worker threads which noticed that they have been friendly interrupted.
 */
class WorkerInterruptedException : public WorkerException
{
	public:
		explicit WorkerInterruptedException(const std::string& errorMessage) :
			WorkerException(errorMessage) {};
};

/**
 * For use by class RemoteWorker when a service host worker encountered an error.
 */
class WorkerRemoteException : public WorkerException
{
	public:
		explicit WorkerRemoteException(const std::string& errorMessage) :
			WorkerException(errorMessage) {};
};


#endif /* WORKERS_WORKEREXCEPTION_H_ */
