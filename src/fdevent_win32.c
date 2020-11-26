/*
 * fdevent_win32 - _WIN32 compatibility using awful MS APIs
 *
 * Copyright(c) 2021 Glenn Strauss gstrauss()gluelogic.com  All rights reserved
 * License: BSD 3-clause (same as lighttpd)
 *
 * x86_64-w64-mingw32-gcc -I. -o a.out fdevent_win32.c -lws2_32 -DTEST
 */
#ifdef _WIN32
/* https://docs.microsoft.com/en-us/windows/win32/winprog/using-the-windows-headers */
/* http://web.archive.org/web/20121219084749/http://support.microsoft.com/kb/166474 */
#define WIN32_LEAN_AND_MEAN
#define VC_EXTRALEAN
/* https://docs.microsoft.com/en-us/previous-versions/ms235384(v=vs.100) */
#ifdef _MSC_VER
#define _CRT_NONSTDC_NO_DEPRECATE
#define _CRT_SECURE_NO_DEPRECATE
#endif
#endif /* _WIN32 */

#include "first.h"


#ifdef _WIN32


/*
 * Microsoft: you look foolish needing 10+ syscalls and ~200 lines of code
 *            to accomplish what is 1 syscall and 1 line in *nix (socketpair()),
 *            and taking some ~670 us in Windows instead of ~1 us in *nix.
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>
#ifdef HAVE_AFUNIX_H
#include <afunix.h>
#else
#define UNIX_PATH_MAX 108
typedef struct sockaddr_un
{
     ADDRESS_FAMILY sun_family;     /* AF_UNIX */
     char sun_path[UNIX_PATH_MAX];  /* pathname */
} SOCKADDR_UN, *PSOCKADDR_UN;
#endif
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "fdevent.h"
#include "ck.h"         /* ck_calloc() */


int fdevent_socketpair_cloexec (int domain, int typ, int protocol, int sv[2])
{
    struct sockaddr_storage ss;
    struct sockaddr *addr = (struct sockaddr *)&ss;
    socklen_t addrlen;

    if (NULL == sv) {
        WSASetLastError(WSAEINVAL);
        return SOCKET_ERROR;
    }
    sv[0] = sv[1] = -1;

    if (domain == AF_UNIX) {
        struct sockaddr_un *un = (struct sockaddr_un *)&ss;
        memset(un, 0, sizeof(struct sockaddr_un));
        un->sun_family = AF_UNIX;
      #if 0 /* Windows does not support connect() to abstract AF_UNIX sockets */
        /* https://github.com/microsoft/WSL/issues/4240 */
        /* XXX: TODO: generate random address (use stack addr?) */
        char sun_path[] = "\0./abcd";
        addrlen = 2+sizeof(sun_path); /*(include trailing '\0')*/
        memcpy(un->sun_path, sun_path, addrlen-2);
      #else
        if (0 != tmpnam_s(un->sun_path, UNIX_PATH_MAX))
            return SOCKET_ERROR;
        addrlen = 2+strlen(un->sun_path)+1;
        if (un->sun_path[0] == '\\')
            memmove(un->sun_path, un->sun_path+1, --addrlen - 2);
      #endif
    }
    else if (domain == AF_INET) {
        /* TCP/IP might be faster than filesystem shenanigans with AF_UNIX */
        struct sockaddr_in *in = (struct sockaddr_in *)&ss;
        memset(in, 0, sizeof(struct sockaddr_in));
        in->sin_family = AF_INET;
        in->sin_port = 0;
        in->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addrlen = sizeof(struct sockaddr_in);
    }
    else if (domain == AF_INET6) {
        /* TCP/IP might be faster than filesystem shenanigans with AF_UNIX */
        struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)&ss;
        memset(in6, 0, sizeof(struct sockaddr_in6));
        in6->sin6_family = AF_INET6;
        in6->sin6_port = 0;
        memcpy(&in6->sin6_addr, &in6addr_loopback, sizeof(struct in6_addr));
        addrlen = sizeof(struct sockaddr_in6);
    }
    else {
        WSASetLastError(WSAEAFNOSUPPORT);
        return SOCKET_ERROR;
    }

    /* _WIN32: Temporarily listen() on constructed addr.
     *         Then connect() to self to create socketpair.
     */

    SOCKET lfd;
    SOCKET fds[2] = { INVALID_SOCKET, INVALID_SOCKET };

    do {

        /* !!! sockets are blocking by default !!!
         * (_WIN32 does not have the equivalent of SOCK_NONBLOCK) */

        /* set up listener */

        /* WSA_FLAG_NO_HANDLE_INHERIT has similar effect to unix SOCK_CLOEXEC */
        /* WSA_FLAG_NO_HANDLE_INHERIT available since Windows 7 Service Pack 1*/
        lfd = WSASocket(addr->sa_family, typ, protocol, NULL, 0,
                        WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
        if (INVALID_SOCKET == lfd)
            break;
      #if 0
        if (addr->sa_family != AF_UNIX) {
            /* SO_REUSEADDR on AF_UNIX results in connect() fail WSAOPNOTSUPP */
            /* SO_REUSEADDR on AF_INET or AF_INET6 is not desirable here since
             * kernel assigned port and we do not want others to be able to bind
             * to this address and port while we are listening */
            int v = 1;
            if (0!=setsockopt(lfd,SOL_SOCKET,SO_REUSEADDR,(char *)&v,sizeof(v)))
                break;
        }
      #endif
      #if 0 /* should not be necessary on path generated by tmpname_s() */
        /*(bind() below should fail if path exists)*/
        if (addr->sa_family == AF_UNIX
            && ((struct sockaddr_un *)addr)->sun_path[0] != '\0') {
            if (0 != DeleteFile(((struct sockaddr_un *)addr)->sun_path)
                && ERROR_FILE_NOT_FOUND != GetLastError())
                break;
        }
      #endif

        if (0 != bind(lfd, addr, addrlen))
            break;
        if (0 != listen(lfd, 1))/*(backlog explicit 1 for a bit more security)*/
            break;

        /* retrieve port assigned by kernel if AF_INET or AF_INET6 port == 0 */
        if ((addr->sa_family == AF_INET
             && 0 == ((struct sockaddr_in *)addr)->sin_port)
            || (addr->sa_family == AF_INET6
                && 0 == ((struct sockaddr_in6 *)addr)->sin6_port)) {
            if (0 != getsockname(lfd, addr, &addrlen))
                break;
        }

        /* connect to listener (create first half of socketpair) */

        /* listen() backlog 1 so that if malicious actor races to connect()
         * to listen() socket (established on unused port by kernel), then
         * (blocking) connect() here should fail with WSAECONNREFUSED.
         * For similar reason, a persistent socketpair() server would have to
         * take additional precautions to prevent misuse, e.g. by requiring
         * client to pass an auth token secret upon connect()
         * (e.g. secret auth token could be a 64-bit number (8-bytes)
         *  of secret random data generated here and validated here)
         * Still, a malicious user on the system could send flood of connection
         * requests to DoS the listening socket unless Windows Firewall was also
         * configured to only permit connections from the same user as the user
         * holding the listening socket
         * Note: intentionally create fds[0] without WSA_FLAG_OVERLAPPED since
         * this function is intended for use with lighttpd mod_cgi.
         * I/O redirection to sockets of MS standard input and output handles
         * works only when the sockets are non-overlapped (and inheritable, but
         * inheritability can be added back later with SetHandleInformation())*/
        fds[0] = WSASocket(addr->sa_family, typ, protocol, NULL, 0,
                           /*WSA_FLAG_OVERLAPPED |*/WSA_FLAG_NO_HANDLE_INHERIT);
        if (INVALID_SOCKET == fds[0])
            break;
        /* XXX: AF_UNIX abstract socket path does not work; fails WSAEINVAL
         * https://github.com/microsoft/WSL/issues/4240 */
        if (0 != connect(fds[0], addr, addrlen))
            break;

        /* accept connection (create second half of socketpair) */

        /* WSA_FLAG_NO_HANDLE_INHERIT state may be inherited from listen()
         * socket state, but that behavior is not documented according to
         * https://curl.se/mail/lib-2019-12/0008.html */
        fds[1] = accept(lfd, NULL, NULL);
        if (INVALID_SOCKET == fds[1])
            break;
        /*(race condition after accept() exists if noinherit not already set)*/
        SetHandleInformation((HANDLE)fds[1], HANDLE_FLAG_INHERIT, 0);

        closesocket(lfd);
        sv[0] = (int)fds[0];
        sv[1] = (int)fds[1];
        if (addr->sa_family == AF_UNIX
            && ((struct sockaddr_un *)addr)->sun_path[0] != '\0')
            DeleteFile(((struct sockaddr_un *)addr)->sun_path);
        return 0;

    } while (0);

    int errnum = WSAGetLastError();
    if (INVALID_SOCKET != lfd)    closesocket(lfd);
    if (INVALID_SOCKET != fds[0]) closesocket(fds[0]);
    if (INVALID_SOCKET != fds[1]) closesocket(fds[1]);
    if (addr->sa_family == AF_UNIX
        && ((struct sockaddr_un *)addr)->sun_path[0] != '\0')
        DeleteFile(((struct sockaddr_un *)addr)->sun_path);
    WSASetLastError(errnum);
    return SOCKET_ERROR;
}


int fdevent_socketpair_nb_cloexec (int domain, int typ, int protocol, int sv[2])
{
    if (0 != fdevent_socketpair_cloexec(domain, typ, protocol, sv))
        return SOCKET_ERROR;

    /* nonblocking sockets */
    u_long ul;
    if (0 == ioctlsocket((SOCKET)(uint64_t)sv[0], FIONBIO, (ul = 1, &ul))
     && 0 == ioctlsocket((SOCKET)(uint64_t)sv[1], FIONBIO, (ul = 1, &ul)))
        return 0;

    int errnum = WSAGetLastError();
    closesocket((SOCKET)(uint64_t)sv[0]);
    closesocket((SOCKET)(uint64_t)sv[1]);
    WSASetLastError(errnum);
    sv[0] = sv[1] = -1;
    return SOCKET_ERROR;
}


int fdevent_socket_set_cloexec (int fd)
{
    return SetHandleInformation((HANDLE)(uint64_t)fd,
                                HANDLE_FLAG_INHERIT, 0) ? 0 : -1;
}


int fdevent_socket_clr_cloexec (int fd)
{
    return SetHandleInformation((HANDLE)(uint64_t)fd,
                                HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)
      ? 0
      : -1;
}


int fdevent_socket_set_nb (int fd)
{
    u_long ul = 1;
    return ioctlsocket(fd, FIONBIO, &ul);
}


int fdevent_socket_set_nb_cloexec (int fd)
{
    return SetHandleInformation((HANDLE)(uint64_t)fd, HANDLE_FLAG_INHERIT, 0)
      ? fdevent_socket_set_nb(fd)
      : -1;
}


int fdevent_socket_cloexec (int domain, int type, int protocol)
{
    return WSASocketA(domain, type, protocol, NULL, 0,
                      WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
}


int fdevent_socket_nb_cloexec (int domain, int type, int protocol)
{
    SOCKET fd = WSASocketA(domain, type, protocol, NULL, 0,
                           WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (fd != INVALID_SOCKET) {
        if (0 != fdevent_socket_set_nb(fd)) {
            closesocket(fd);
            fd = INVALID_SOCKET; /* INVALID_SOCKET is (unsigned long long)-1 */
        }
    }
    return fd;
}


void fdevent_setfd_cloexec (int fd)
{
    SetHandleInformation((HANDLE)_get_osfhandle(fd),
                         HANDLE_FLAG_INHERIT, 0);
}


void fdevent_clrfd_cloexec (int fd)
{
    SetHandleInformation((HANDLE)_get_osfhandle(fd),
                         HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
}


int fdevent_fcntl_set_nb (int fd)
{
    DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
    return SetNamedPipeHandleState((HANDLE)_get_osfhandle(fd),
                                   &mode, NULL, NULL) ? 0 : -1;
}


int fdevent_fcntl_set_nb_cloexec (int fd)
{
    fdevent_setfd_cloexec(fd);
    return fdevent_fcntl_set_nb(fd);
}


int fdevent_fcntl_set_nb_cloexec_sock (int fd)
{
    /*(should have created listening sockets non-blocking and no-inherit)*/
    UNUSED(fd);
    return 0;
}


#include <windows.h>
#include <signal.h>     /* sig_atomic_t */

#ifndef INFINITE
#define INFINITE      0xFFFFFFFF
#endif

#ifndef WAIT_OBJECT_0
#define WAIT_OBJECT_0 0x00000000L
#endif

#ifndef WAIT_TIMEOUT
#define WAIT_TIMEOUT  0x00000102L
#endif

static struct pilist {
  struct pilist *next;
  HANDLE hProcess;
  HANDLE hWait;
  pid_t pid;
  int ran;
} *pilist;

static volatile sig_atomic_t *fdevent_sig_child;

void fdevent_win32_init (volatile sig_atomic_t *ptr);
void fdevent_win32_init (volatile sig_atomic_t *ptr)
{
    fdevent_sig_child = ptr;
}


void fdevent_win32_cleanup (void)
{
    struct pilist *pi, *next = pilist;
    while ((pi = next)) {
        next = pi->next;
        if (pi->hWait != INVALID_HANDLE_VALUE)
            /*(could block, but should not; our callback func is simple)*/
            UnregisterWaitEx(pi->hWait, INVALID_HANDLE_VALUE);
        if (pi->hProcess != INVALID_HANDLE_VALUE)
            /*(could check for exit, send Ctrl-Break or TerminateProcess)*/
            CloseHandle(pi->hProcess);
        free(pi);
    }
}


int fdevent_waitpid (pid_t pid, int * const status, int nb)
{

  #if 0

    if (-1 == pid) {
        errno = ENOTSUP;
        return -1;
    }

    HANDLE hProcess = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!hProcess) {
        errno = ECHILD;
        return -1;
    }
    DWORD dw = WaitForSingleObject(hProcess, nb ? 0 : INFINITE);

    if (WAIT_OBJECT_0 == dw) {
        /*(GetExitCodeProcess() failure not expected; not handled)*/
        if (status)
            *status = GetExitCodeProcess(hProcess,&dw) ? ((dw & 0xff) << 8) : 0;
    }
    else if (WAIT_TIMEOUT == dw && nb) {
        pid = 0;
    }
    else {
        errno = ECHILD;
        pid = (pid_t)-1;
    }

    CloseHandle(hProcess);
    return pid;

  #else

    /* note: not thread-safe; not efficient with many processes (O(n)) */

    struct pilist *pi;
    struct pilist **next = &pilist;
    if (-1 == pid) {
        while ((pi = *next) && !pi->ran)
            next = &pi->next;
        if (pi)
            pid = pi->pid;
        else if (pilist)
            return 0;
        /*(else ECHILD below)*/
    }
    else {
        while ((pi = *next) && pid != pi->pid)
            next = &pi->next;
    }
    if (NULL == pi) {
        errno = ECHILD;
        return -1;
    }

    HANDLE hProcess = pi->hProcess;
    DWORD dw = pi->ran
      ? WAIT_OBJECT_0
      : WaitForSingleObject(hProcess, nb ? 0 : INFINITE);

    if (WAIT_OBJECT_0 == dw) {
        /*(GetExitCodeProcess() failure not expected; not handled)*/
        if (status)
            *status = GetExitCodeProcess(hProcess,&dw) ? ((dw & 0xff) << 8) : 0;
    }
    else if (WAIT_TIMEOUT == dw && nb) {
        return 0;
    }
    else {
        errno = ECHILD;
        pid = -1;
    }

    if (pi->hWait != INVALID_HANDLE_VALUE)
        /*(could block, but should not; our callback func is simple)*/
        UnregisterWaitEx(pi->hWait, INVALID_HANDLE_VALUE);

    *next = pi->next;
    free(pi);
    CloseHandle(hProcess);
    return pid;

  #endif
}


int fdevent_waitpid_intr (pid_t pid, int * const status)
{
    return fdevent_waitpid(pid, status, 0);
}


static void CALLBACK fdevent_WaitOrTimerCallback (PVOID lpParameter, BOOLEAN TimerOrWaitFired)
{
    UNUSED(TimerOrWaitFired);
    struct pilist *pi = lpParameter;
  #if 0
    /* (do not make blocking call to UnregisterWaitEx() from within callback) */
    UnregisterWaitEx(pi->hWait, NULL);/*non-blocking with NULL CompletionEvent*/
    /* note: safe to ignore UnregisterWaitEx() return value error with
     * GetLastError() ERROR_IO_PENDING) since we use RegisterWaitForSingleObject
     * with WT_EXECUTEONLYONCE, and so there can be no other possible queued
     * events for pi->hWait, other than this currently running func */
    pi->hWait = INVALID_HANDLE_VALUE;
  #endif
    pi->ran = 1;
    if (fdevent_sig_child) *fdevent_sig_child = 1;
}


/* sorting the environment block
 * https://learn.microsoft.com/en-us/windows/win32/procthread/changing-environment-variables
 * https://learn.microsoft.com/en-us/windows/win32/procthread/creating-processes
 *  "All strings in the environment block must be sorted alphabetically by name.
 *   The sort is case-insensitive, Unicode order, without regard to locale."
 * https://learn.microsoft.com/en-us/windows/win32/intl/handling-sorting-in-your-applications
 *   (could use CompareStringOrdinal() for ordinal sort independent of locale,
 *    but CompareStringOrdinal() takes LPCWCH params, so we use _stricmp())
 * XXX: technically, sorting env block should be done on env var name (label)
 *      up to the equal sign, and not beyond, but that is not done here.
 *      To be more pedantically correct, caller could temporarily find '=',
 *      reject strings missing '=', and set '=' to '\0' on all envp[]; then call
 *      qsort(); and finally restore '=' on all envp[] after qsort() returns.
 */
static int fdevent_envcmpfunc (const void *a, const void *b)
{
    return _stricmp(*(const char **)a, *(const char **)b);
}


pid_t fdevent_createprocess (char *argv[], char *envp[], intptr_t fdin, intptr_t fdout, int fderr, int dfd)
{
    /*
     * The Microsoft CreateProcess() interface is criminally broken.
     * Forcing argument strings to be concatenated into a single string
     * only to be re-parsed by Windows can lead to security issues.
     *
     * NB: callers of fdevent_createprocess() must properly escape and quote
     *     arguments appropriately for target program (argv[0]) so that target
     *     program can safely parse command line after calling GetCommandLine()
     * NB: potential security exposure from mod_ssi arguments
     *     e.g. constructed path names from user-provided info (url-path)
     */

    size_t len = 0;
    char *dirp = NULL;
    if (0 == strcmp(argv[0],"/bin/sh") && argv[1] && 0 == strcmp(argv[1],"-c")){
        /* future: consider checking for SHELL variable in environment */
      #if 1
        *(const char **)&argv[0] = "C:\\Windows\\System32\\cmd.exe";
        *(const char **)&argv[1] = "/c";
      #else
        *(const char **)&argv[0] =
          "C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
      #endif
    }
    else if (dfd <= -2) { /*(flag to chdir to directory containing argv[x])*/
        /* dfd == -2 for argv[0], dfd == -3 indicates argv[1] */
        const char *arg = (-3 == dfd) ? argv[1] : argv[0];
        if (arg && (arg[0] == '\\' || arg[0] == '/' || arg[1] == ':')) {
            /* step over "/cygdrive/c/..." (or other drive letter) -> "/..."
             * (note: below code assumes script is on current volume)
             * (to honor volume, could copy volume and replace '/' with ':') */
            if (0 == memcmp(arg, "/cygdrive/", 10)
                && arg[10] != '\0' && arg[11] == '/')
                arg += 11;

            char *sl = strrchr(arg, '/');
            char *bs = strrchr(arg, '\\');
            if (sl < bs) sl = bs;
            if (sl && sl != arg) {
                len = (size_t)(sl - arg);
                dirp = malloc(len+1);
                if (NULL == dirp)
                    return -1;
                memcpy(dirp, arg, len);
                dirp[len] = '\0';
            }
        }
    } /* else expecting (-1 == dfd); this code does not handle open dirfd */

    char *args = NULL;
    len = 0;
    for (int i = 0; argv[i]; ++i)
        len += strlen(argv[i]) + 1;
    if (len) {
        args = malloc(len);
        if (NULL == args) {
            free(dirp);
            return -1;
        }
        size_t off = 0;
        for (int i = 0; argv[i]; ++i) {
            len = strlen(argv[i]);
            memcpy(args+off, argv[i], len);
            off += len;
            args[off++] = ' ';
        }
        args[off-1] = '\0'; /*(remove trailing space)*/
    }

    char *envs = NULL;
    if (envp) {
        int i;
        len = 0;
        for (i = 0; envp[i]; ++i)
            len += strlen(envp[i]) + 1;
        if (len) { /* MS env block size limit is SHRT_MAX (32767) */
            qsort(envp, (size_t)i, sizeof(char *), fdevent_envcmpfunc);
            if (++len > 32767 || NULL == (envs = malloc(len))) {
                free(dirp);
                free(args);
                return -1;
            }
            size_t off = 0;
            for (i = 0; envp[i]; ++i) {
                len = strlen(envp[i]) + 1; /*(include '\0')*/
                memcpy(envs+off, envp[i], len);
                off += len;
            }
            envs[off] = '\0';
        }
    }

    STARTUPINFOEXA info;
    memset(&info, 0, sizeof(info));
    LPSTARTUPINFOA sinfo = &info.StartupInfo;
    sinfo->cb = sizeof(STARTUPINFOEX);
    sinfo->lpDesktop = NULL;
    sinfo->lpTitle = argv[0];
    sinfo->dwFlags = STARTF_FORCEOFFFEEDBACK | STARTF_USESTDHANDLES
                   | STARTF_USESHOWWINDOW | EXTENDED_STARTUPINFO_PRESENT;
    sinfo->wShowWindow = SW_HIDE;

    /* limit handles inherited by new process to specified stdin,stdout,stderr
     *
     * Programmatically controlling which handles are inherited by new processes
     * in Win32: https://devblogs.microsoft.com/oldnewthing/20111216-00/?p=8873
     */
    size_t sz = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &sz);
    /* GetLastError() == ERROR_INSUFFICIENT_BUFFER */
    LPPROC_THREAD_ATTRIBUTE_LIST attrlist = info.lpAttributeList = malloc(sz);
    if (NULL == attrlist
        || !InitializeProcThreadAttributeList(attrlist, 1, 0, &sz)
           /* (reuse part of STARTUPINFOA for (HANDLE) list of three handles) */
        || (UpdateProcThreadAttribute(attrlist, 0,
                                      PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                      &sinfo->hStdInput, 3*sizeof(HANDLE),
                                      NULL, NULL)
             ? 0 : (DeleteProcThreadAttributeList(attrlist), 1))) {
        free(attrlist);
        free(envs);
        free(args);
        free(dirp);
        return -1;
    }

  #if 0
    /* https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/get-osfhandle?view=msvc-170
     * use _fileno() on standard streams with _get_osfhandle();
     *   avoid STD*_FILENO constants here */
    sinfo->hStdInput =(HANDLE)_get_osfhandle(fdin >=0 ? fdin  :_fileno(stdin));
    sinfo->hStdOutput=(HANDLE)_get_osfhandle(fdout>=0 ? fdout :_fileno(stdout));
    sinfo->hStdError =(HANDLE)_get_osfhandle(fderr>=0 ? fderr :_fileno(stderr));
  #endif
    if (dfd <= -2) { /* overloaded flag means _WIN32 SOCKET on fdin,fdout */
        sinfo->hStdInput = fdin  != -1 ? (HANDLE)fdin
                                       : GetStdHandle(STD_INPUT_HANDLE);
        sinfo->hStdOutput= fdout != -1 ? (HANDLE)fdout
                                       : GetStdHandle(STD_OUTPUT_HANDLE);
    }
    else {
        sinfo->hStdInput = fdin  >= 0 ? (HANDLE)_get_osfhandle((int)fdin)
                                      : GetStdHandle(STD_INPUT_HANDLE);
        sinfo->hStdOutput= fdout >= 0 ? (HANDLE)_get_osfhandle((int)fdout)
                                      : GetStdHandle(STD_OUTPUT_HANDLE);
    }
    sinfo->hStdError = fderr >= 0 ? (HANDLE)_get_osfhandle(fderr)
                                  : GetStdHandle(STD_ERROR_HANDLE);

    /* paranoia: all handles should be created NOINHERIT
     * in case third-party code is not careful when calling CreateProcess()
     * There is still a race condition setting these handles inheritable,
     * but handles must be inheritable for use with STARTF_USESTDHANDLES */
    if (sinfo->hStdInput != INVALID_HANDLE_VALUE)
        SetHandleInformation(sinfo->hStdInput,
                             HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    if (sinfo->hStdOutput!= INVALID_HANDLE_VALUE)
        SetHandleInformation(sinfo->hStdOutput,
                             HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    if (sinfo->hStdError != INVALID_HANDLE_VALUE)
        SetHandleInformation(sinfo->hStdError,
                             HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

    PROCESS_INFORMATION pinfo;
    memset(&pinfo, 0, sizeof(pinfo));
    pinfo.hProcess = INVALID_HANDLE_VALUE;
    pinfo.hThread  = INVALID_HANDLE_VALUE;
    BOOL bInheritHandles  = TRUE;
    DWORD dwCreationFlags = NORMAL_PRIORITY_CLASS
                          | CREATE_NO_WINDOW
                          | CREATE_NEW_PROCESS_GROUP;
  #ifdef UNICODE
    dwCreationFlags |= CREATE_UNICODE_ENVIRONMENT;
  #endif
    pid_t pid = (pid_t)-1;
    if (CreateProcessA(argv[0], args, NULL, NULL, bInheritHandles,
                       dwCreationFlags, envs, dirp, sinfo, &pinfo)) {
        CloseHandle(pinfo.hThread);
        struct pilist *pi = ck_calloc(1, sizeof(*pi));
        pi->hProcess  = pinfo.hProcess;
        pi->pid = pid = (pid_t)pinfo.dwProcessId;
        pi->hWait     = INVALID_HANDLE_VALUE;
        if (!RegisterWaitForSingleObject(&pi->hWait, pi->hProcess,
                                         fdevent_WaitOrTimerCallback, pi,
                                         INFINITE, WT_EXECUTEONLYONCE)) {
            /* TODO: GetLastError(); maybe hit thread pool limit (default 500)*/
            /* failure consequence is no event received when process exits;
             * still able to WaitForSingleObject() and signal to terminate */
        }
        /* note: not thread-safe */
        pi->next = pilist;
        pilist = pi;
    }
    else {
      #if 0
        TCHAR lpMsgBuf[1024];
        FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, NULL, GetLastError(),
                      0, /* MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT) */
                      (LPTSTR)lpMsgBuf, sizeof(lpMsgBuf)/sizeof(TCHAR), NULL);
        fprintf(stderr, "CreateProcess() %s: %s", args, lpMsgBuf);
      #endif
        /* (quiet VS Analyzer which thinks hProcess might leak w/o this) */
        if (pinfo.hProcess != INVALID_HANDLE_VALUE)
            CloseHandle(pinfo.hProcess);
        if (pinfo.hThread != INVALID_HANDLE_VALUE)
            CloseHandle(pinfo.hThread);
    }

    /* paranoia: all handles should be created NOINHERIT
     * in case third-party code is not careful when calling CreateProcess()
     * There is still a race condition setting these handles inheritable,
     * but handles must be inheritable for use with STARTF_USESTDHANDLES */
    if (sinfo->hStdInput != INVALID_HANDLE_VALUE)
        SetHandleInformation(sinfo->hStdInput,  HANDLE_FLAG_INHERIT, 0);
    if (sinfo->hStdOutput!= INVALID_HANDLE_VALUE)
        SetHandleInformation(sinfo->hStdOutput, HANDLE_FLAG_INHERIT, 0);
    if (sinfo->hStdError != INVALID_HANDLE_VALUE)
        SetHandleInformation(sinfo->hStdError,  HANDLE_FLAG_INHERIT, 0);

    DeleteProcThreadAttributeList(attrlist);
    free(attrlist);
    free(envs);
    free(args);
    free(dirp);

    return pid;
}


int fdevent_dup_cloexec (int fd)
{
    const int newfd = _dup(fd);
    if (newfd >= 0) fdevent_setfd_cloexec(newfd);
    return newfd;
}


#include <fcntl.h>

int fdevent_open_cloexec (const char *pathname, int symlinks, int flags, mode_t mode)
{
    UNUSED(symlinks);
    return _open(pathname, flags | _O_BINARY | _O_NOINHERIT, mode);
}


int fdevent_open_devnull (void)
{
    return fdevent_open_cloexec("nul:", 0, O_RDWR, 0);
}


#if 0 /* not currently used on _WIN32 */
int fdevent_open_dirname (char *path, int symlinks)
{
    /*(handle special cases of no dirname or dirname is root directory)*/
    char *c = strrchr(path, '/');
    char * const bs = strrchr(path, '\\');
    if (c < bs) c = bs;
    const char * const dname = (NULL != c ? c == path ? "/" : path : ".");
    if (NULL != c) *c = '\0';
    int dfd = fdevent_open_cloexec(dname, symlinks, _O_RDONLY, 0);
    if (NULL != c) *c = (c == bs) ? '\\' : '/';
    return dfd;
}
#endif


int fdevent_pipe_cloexec (int * const fds, const unsigned int bufsz_hint)
{
    return _pipe(fds, bufsz_hint, _O_BINARY | _O_NOINHERIT);
}


int fdevent_socket_close (int fd)
{
    return closesocket(fd);
}


int fdevent_accept_listenfd (int listenfd, struct sockaddr *addr, size_t *addrlen)
{
    socklen_t len = (socklen_t) *addrlen;
    SOCKET fd = accept(listenfd, addr, &len);
    if (fd == INVALID_SOCKET)
        return -1;
    *addrlen = (size_t)len;
   #if 0 /*WSA_FLAG_NO_HANDLE_INHERIT and non-blocking inherited from listenfd*/
    if (0 != fdevent_socket_set_nb_cloexec(fd)) {
        closesocket(fd);
        return -1;
    }
   #endif
   #if 0
    if (fd > INT_MAX)
        fprintf(stderr, "warning: SOCKET fd > INT_MAX (fd:%llu)", fd);
   #endif
    return (int)fd;
}


char ** fdevent_environ (void)
{
    return environ;
}


#include <sys/stat.h>   /* _S_IREAD _S_IWRITE */
#include <share.h>      /* _SH_DENYRW */
int fdevent_mkostemp (char *path, int flags)
{
    /* notes:
     * - _O_TEMPORARY omitted since file deleted if all handles closed,
     *   but close may occur in chunkqueue with large request/response using
     *   multiple temp files
     * - XXX: might convert path from UTF-8 to wide-char
     */
    char *p;
    for (p = path; *p; ++p) if (*p == '\\') *p = '/';
    int fd;      /*(flags might have _O_APPEND)*/
    flags |= _O_RDWR | _O_CREAT | _O_EXCL | _O_BINARY | _O_NOINHERIT;
    return (0 == _mktemp_s(path, (size_t)(p - path + 1)))
        && (0 == _sopen_s(&fd, path, flags, _SH_DENYRW, _S_IREAD | _S_IWRITE))
      ? fd
      : -1;
    /* future: if _sopen_s() returns EEXIST, might reset template (path) with
     * trailing "XXXXXX", and then loop to try again */
}


int fdevent_rename (const char *oldpath, const char *newpath)
{
    /* MoveFileExA() vs ReplaceFileA() difference should not matter for use in
     * caches, e.g. mod_deflate and mod_dirlisting, but ReplaceFileA() may be
     * preferred in other places, such as mod_webdav. */
  #if 1
    return MoveFileExA(oldpath, newpath,
                         MOVEFILE_COPY_ALLOWED
                       | MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
  #else
    return ReplaceFileA(newpath, oldpath, NULL,
                          REPLACEFILE_IGNORE_MERGE_ERRORS
                        | REPLACEFILE_IGNORE_ACL_ERRORS, NULL, NULL) ? 0 : -1;
  #endif
}


ssize_t fdevent_socket_read_discard (int fd, char *buf, size_t sz, int family, int so_type)
{
    UNUSED(family);
    UNUSED(so_type);
    ssize_t rd = recv(fd, buf, sz, 0);
    if (rd == SOCKET_ERROR) {
        switch (WSAGetLastError()) {
          case WSAEINTR:       errno = EINTR;  break;
          case WSAEWOULDBLOCK: errno = EAGAIN; break;
          default:             errno = EIO;    break;
        }
    }
    return rd;
}


#include "sys-socket.h" /*(custom definition for S_IFSOCK)*/
int fdevent_ioctl_fionread (int fd, int fdfmt, int *toread)
{
    if (fdfmt != S_IFSOCK) { errno = ENOTSOCK; return -1; }
    u_long l;
    int rc = ioctlsocket(fd, FIONREAD, &l);
    if (0 == rc) *toread = (int)l;
    return rc;
}


#ifdef TEST
int main (void)
{
    WSADATA wsaData;
    if (0 != WSAStartup(MAKEWORD(2, 2), &wsaData))
        return -1;

    int sv[2];
    int rc = 0 == fdevent_socketpair_cloexec(AF_INET, SOCK_STREAM, 0, sv)
          && 0 == fdevent_socketpair_cloexec(AF_UNIX, SOCK_STREAM, 0, sv);

    WSACleanup();
    return rc ? 0 : -1;
}
#endif


#endif /* _WIN32 */
