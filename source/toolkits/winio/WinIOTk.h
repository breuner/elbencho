// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef TOOLKITS_WINIO_WINIOTK_H_
#define TOOLKITS_WINIO_WINIOTK_H_

#include <cstdint>
#include <fcntl.h>
#include <string>

/**
 * Stateless Win32 helpers for the native Windows I/O path of Cygwin builds (CYGWIN_SUPPORT).
 * Nothing in here is on the hot I/O path. Handles are passed around as void*, so that no caller
 * needs <windows.h>. Failing functions set errno (mapped from the Win32 error) and leave the Win32
 * error untouched, so lastErrorStr() can still be called afterwards for the message text.
 */
class WinIOTk
{
    public:
        static bool isWinDevicePath(const std::string& path);
        static std::wstring toWinPath(const std::string& path);

        static void* openFile(const std::string& path, int posixOpenFlags);
        static bool closeFile(void* handle);

        static uint64_t getBlockDevSize(const std::string& path);
        static bool setFileSize(void* handle, uint64_t len);
        static bool setSparse(void* handle);
        static bool preallocate(void* handle, uint64_t offset, uint64_t len);
        static bool statFile(void* handle);

        static int errnoFromLastError();
        static std::string lastErrorStr();

        // path-based variants for files that are not open through this class (e.g. in ProgArgs)

        static bool setFileSize(const std::string& path, uint64_t len)
            { return openAndApply(path, [&](void* handle) { return setFileSize(handle, len); } ); }
        static bool preallocate(const std::string& path, uint64_t offset, uint64_t len)
            { return openAndApply(path,
                [&](void* handle) { return preallocate(handle, offset, len); } ); }

    private:
        /**
         * Open the given path, apply func(handle) and close it again.
         *
         * @return result of func; false on open error. errno is set on error.
         */
        template <class FUNC>
        static bool openAndApply(const std::string& path, FUNC func)
        {
            void* handle = openFile(path, O_RDWR);
            if(!handle)
                return false;

            const bool funcRes = func(handle);
            const int funcErrno = errno;

            closeFile(handle);

            errno = funcErrno;
            return funcRes;
        }
};

#endif /* TOOLKITS_WINIO_WINIOTK_H_ */
