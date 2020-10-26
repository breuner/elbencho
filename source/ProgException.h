#ifndef PROGEXCEPTION_H_
#define PROGEXCEPTION_H_

#include <string>
#include <iostream>
#include <exception>

/**
 * For errors with explanation message in main (coordinator) thread.
 */
class ProgException : public std::exception
{
	public:
		explicit ProgException(const std::string& errorMessage) : errorMessage(errorMessage) {};


	private:
		std::string errorMessage;


	// inliners
	public:
		virtual const char* what() const throw() { return errorMessage.c_str(); }
};

/**
 * For interruption by signal in main (coordinator) thread, e.g. from user pressing ctrl+c.
 */
class ProgInterruptedException : public ProgException
{
	public:
		explicit ProgInterruptedException(const std::string& errorMessage) :
			ProgException(errorMessage) {};
};

/**
 * For expired phase time limit in main (coordinator) thread.
 */
class ProgTimeLimitException : public ProgException
{
	public:
		explicit ProgTimeLimitException(const std::string& errorMessage) :
			ProgException(errorMessage) {};
};

#endif /* PROGEXCEPTION_H_ */
