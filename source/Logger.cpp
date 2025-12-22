// SPDX-FileCopyrightText: 2020-2025 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "Logger.h"

std::mutex LoggerBase::mutex;
std::stringstream LoggerBase::errHistoryStream;
bool LoggerBase::keepErrHistory = true;
LogLevel LoggerBase::filterLevel = Log_NORMAL;

