// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef HTTPLIB_H_
#define HTTPLIB_H_

// The one place that includes cpp-httplib, so that all translation units of all tools see the
// same settings.
#define CPPHTTPLIB_RECV_BUFSZ (256u * 1024u) // fewer, larger chunks for uploaded object data
#include <httplib.h>

#endif // HTTPLIB_H_
