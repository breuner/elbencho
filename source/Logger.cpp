#include "Logger.h"

std::mutex LoggerBase::mutex;
std::stringstream LoggerBase::errHistoryStream;
bool LoggerBase::keepErrHistory = true;
LogLevel LoggerBase::filterLevel = Log_NORMAL;

