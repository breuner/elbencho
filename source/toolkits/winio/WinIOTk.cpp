// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifdef CYGWIN_SUPPORT

#include <algorithm>
#include <cerrno>
#include <cwchar>
#include <fcntl.h>
#include <sys/cygwin.h>

#include "WinIOTk.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>


/**
 * @return true if path is a native Windows device name like "\\.\PhysicalDrive2" (also accepted
 *      with forward slashes, which saves the quoting in a shell).
 */
bool WinIOTk::isWinDevicePath(const std::string& path)
{
    return !path.rfind("\\\\.\\", 0) || !path.rfind("//./", 0);
}

/**
 * Convert a path as given on the command line to what CreateFileW() expects: native device names
 * only get their slashes normalized, everything else (incl. the cygwin device names like
 * "/dev/sdc") goes through cygwin's own path conversion.
 *
 * @return empty string on error, in which case errno is set.
 */
std::wstring WinIOTk::toWinPath(const std::string& path)
{
    if(isWinDevicePath(path) )
    {
        std::wstring winPath(path.begin(), path.end() );
        std::replace(winPath.begin(), winPath.end(), L'/', L'\\');
        return winPath;
    }

    const cygwin_conv_path_t what = CCP_POSIX_TO_WIN_W | CCP_ABSOLUTE;

    const ssize_t bufSize = cygwin_conv_path(what, path.c_str(), NULL, 0); // incl. terminator
    if(bufSize < 0)
        return L"";

    std::wstring winPath(bufSize / sizeof(wchar_t), L'\0');

    if(cygwin_conv_path(what, path.c_str(), &winPath[0], bufSize) )
        return L"";

    winPath.resize(wcslen(winPath.c_str() ) );

    return winPath;
}

/**
 * Open a file or block device for the benchmark data path. The handle is always opened for
 * overlapped I/O (see WinIO for why) and shared, like the fds that cygwin opens.
 *
 * @posixOpenFlags the flags that the caller would hand to open(); O_RDWR/O_RDONLY, O_CREAT, O_TRUNC
 *      and O_DIRECT are mapped, the rest is ignored.
 * @return nullptr on error, in which case errno is set.
 */
void* WinIOTk::openFile(const std::string& path, int posixOpenFlags)
{
    const std::wstring winPath = toWinPath(path);
    if(winPath.empty() )
        return nullptr;

    const bool isDevice = !winPath.rfind(L"\\\\.\\", 0); // (cygwin converts /dev/sdX to this too)

    DWORD access = GENERIC_READ;
    if( (posixOpenFlags & O_ACCMODE) != O_RDONLY)
        access |= GENERIC_WRITE;

    DWORD creation = OPEN_EXISTING; // (devices can only be opened this way)
    if(!isDevice && (posixOpenFlags & O_CREAT) )
        creation = (posixOpenFlags & O_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS;

    DWORD flags = FILE_FLAG_OVERLAPPED;
    if(posixOpenFlags & O_DIRECT)
        flags |= FILE_FLAG_NO_BUFFERING;

    HANDLE handle = CreateFileW(winPath.c_str(), access,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, creation, flags, NULL);

    if(handle == INVALID_HANDLE_VALUE)
    {
        errno = errnoFromLastError();
        return nullptr;
    }

    return handle;
}

/**
 * @return false on error, in which case errno is set.
 */
bool WinIOTk::closeFile(void* handle)
{
    if(CloseHandle(handle) )
        return true;

    errno = errnoFromLastError();
    return false;
}

/**
 * @return size of the block device in bytes; 0 on error, in which case errno is set.
 */
uint64_t WinIOTk::getBlockDevSize(const std::string& path)
{
    const std::wstring winPath = toWinPath(path);
    if(winPath.empty() )
        return 0;

    HANDLE handle = CreateFileW(winPath.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);

    if(handle == INVALID_HANDLE_VALUE)
    {
        errno = errnoFromLastError();
        return 0;
    }

    GET_LENGTH_INFORMATION lengthInfo = {};
    DWORD numBytesReturned;

    const BOOL ioctlRes = DeviceIoControl(handle, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0,
        &lengthInfo, sizeof(lengthInfo), &numBytesReturned, NULL);
    const int ioctlErrno = ioctlRes ? 0 : errnoFromLastError();

    CloseHandle(handle);

    if(!ioctlRes)
    {
        errno = ioctlErrno;
        return 0;
    }

    return lengthInfo.Length.QuadPart;
}

/**
 * Set end of file, like ftruncate().
 *
 * @return false on error, in which case errno is set.
 */
bool WinIOTk::setFileSize(void* handle, uint64_t len)
{
    FILE_END_OF_FILE_INFO eofInfo;
    eofInfo.EndOfFile.QuadPart = len;

    if(SetFileInformationByHandle(handle, FileEndOfFileInfo, &eofInfo, sizeof(eofInfo) ) )
        return true;

    errno = errnoFromLastError();
    return false;
}

/**
 * Mark the file sparse, which is what an ftruncate() extension on linux (and in cygwin) gives: the
 * unwritten range is a hole, so NTFS doesn't zero-fill up to the offset of a later write.
 *
 * @return false on error, in which case errno is set.
 */
bool WinIOTk::setSparse(void* handle)
{
    FILE_SET_SPARSE_BUFFER sparseBuf = { TRUE };
    DWORD numBytesReturned;

    if(DeviceIoControl(handle, FSCTL_SET_SPARSE, &sparseBuf, sizeof(sparseBuf), NULL, 0,
        &numBytesReturned, NULL) )
        return true;

    errno = errnoFromLastError();
    return false;
}

/**
 * Allocate disk space for the given range and extend the file to its end if it is shorter, like
 * posix_fallocate(). (Note: This cannot move the NTFS valid data length, so writes beyond it still
 * make the file system zero-fill the gap, as on any new file. And it allocates nothing for a sparse
 * file, so it must not be combined with setSparse().)
 *
 * @return false on error, in which case errno is set.
 */
bool WinIOTk::preallocate(void* handle, uint64_t offset, uint64_t len)
{
    const uint64_t rangeEnd = offset + len;

    FILE_ALLOCATION_INFO allocInfo;
    allocInfo.AllocationSize.QuadPart = rangeEnd;

    if(!SetFileInformationByHandle(handle, FileAllocationInfo, &allocInfo, sizeof(allocInfo) ) )
    {
        errno = errnoFromLastError();
        return false;
    }

    LARGE_INTEGER fileSize;

    if(!GetFileSizeEx(handle, &fileSize) )
    {
        errno = errnoFromLastError();
        return false;
    }

    if( (uint64_t)fileSize.QuadPart >= rangeEnd)
        return true;

    return setFileSize(handle, rangeEnd);
}

/**
 * Query the file's metadata, like fstat(). (The result is of no interest to the benchmark, only
 * the operation itself is.)
 *
 * @return false on error, in which case errno is set.
 */
bool WinIOTk::statFile(void* handle)
{
    BY_HANDLE_FILE_INFORMATION fileInfo;

    if(GetFileInformationByHandle(handle, &fileInfo) )
        return true;

    errno = errnoFromLastError();
    return false;
}

/**
 * @return errno value for the current Win32 error, so that the existing strerror(errno) messages
 *      and the EINVAL check for misaligned direct I/O keep working.
 */
int WinIOTk::errnoFromLastError()
{
    switch(GetLastError() )
    {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_NAME: return ENOENT;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
        case ERROR_WRITE_PROTECT: return EACCES;
        case ERROR_INVALID_PARAMETER:
        case ERROR_INVALID_HANDLE: return EINVAL;
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL: return ENOSPC;
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY: return ENOMEM;
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS: return EEXIST;
        case ERROR_OPERATION_ABORTED: return ECANCELED;
        default: return EIO;
    }
}

/**
 * @return message text of the current Win32 error plus the error number.
 */
std::string WinIOTk::lastErrorStr()
{
    const DWORD winErr = GetLastError();
    char* msgBuf = nullptr;

    const DWORD msgLen = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, winErr, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&msgBuf, 0, NULL);

    std::string msg = msgLen ? std::string(msgBuf, msgLen) : std::string("Unknown error");

    LocalFree(msgBuf);

    while(!msg.empty() && ( (msg.back() == '\r') || (msg.back() == '\n') || (msg.back() == '.') ) )
        msg.pop_back();

    return msg + " (Win32 error " + std::to_string(winErr) + ")";
}

#endif // CYGWIN_SUPPORT
