//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "fault.hpp"
#include "fault_slot.hpp"

#include <boost/corosio/detail/platform.hpp>
#include <boost/corosio/host_name.hpp>

#if BOOST_COROSIO_HAVE_LIBURING
#include <liburing.h>
#endif

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <tuple>
#include <unistd.h>

#if defined(__linux__)
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#endif

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/event.h>
#endif

#if defined(__APPLE__)
#include <cstdint>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#endif

// Linux-only open flag; folding it to zero keeps the mode test in the
// `open` shadow one expression on every platform.
#if !defined(O_TMPFILE)
#define O_TMPFILE 0
#endif

namespace boost::corosio::test::fault {

thread_local cqe_slot tls_cqe;

// The arm machinery lives in fault_arm.cpp; this is the one piece of
// it that has to know what platform it is on.
void publish_error(int err) noexcept
{
    errno = err;
}

void* real_symbol(char const* name) noexcept
{
    void* p = ::dlsym(RTLD_NEXT, name);
    if(!p)
    {
        char msg[160];
        std::snprintf(msg, sizeof(msg),
            "fault harness: %s has no implementation behind the shadow",
            name);
        die(msg);
    }
    return p;
}

namespace {

// Copy the prefix of `in` holding at most `count` bytes into `out`.
// Returns the new iovec count. Corosio never passes more than a
// handful of buffers; 64 is a hard ceiling checked at runtime.
int truncate_iov(iovec const* in, int n, std::size_t count, iovec* out) noexcept
{
    if(n > 64)
        die("fault harness: iovec count exceeds 64");
    int m = 0;
    for(; m < n && count > 0; ++m)
    {
        out[m] = in[m];
        if(out[m].iov_len > count)
            out[m].iov_len = count;
        count -= out[m].iov_len;
    }
    return m;
}

} // namespace

cqe_fault_scope::cqe_fault_scope(int fd, int opcode, int res)
{
    claim_completion_slot(tls_cqe,
        "cqe_fault_scope: a completion fault is already armed on this thread");
    tls_cqe.fd = fd;
    tls_cqe.opcode = opcode;
    tls_cqe.res = res;
}

cqe_fault_scope::~cqe_fault_scope()
{
    release_completion_slot(tls_cqe);
}

bool cqe_fault_scope::fired() const noexcept
{
    return tls_cqe.fired;
}

} // boost::corosio::test::fault

using namespace boost::corosio::test::fault;

// Each shadow resolves the real function on first use. dlsym may
// allocate, which is fine: malloc is not shadowed.
#define COROSIO_FAULT_REAL(name, sig) \
    static auto const real = reinterpret_cast<sig>(real_symbol(#name))

// glibc marks part of the census __THROW, which C++ reads as noexcept;
// Darwin's headers carry no exception specification at all and a
// redeclaration that adds one is ill-formed, so the spec is per-OS.
#if defined(__linux__)
#define COROSIO_FAULT_NOTHROW noexcept
#else
#define COROSIO_FAULT_NOTHROW
#endif

extern "C" int socket(int domain, int type, int protocol) COROSIO_FAULT_NOTHROW
{
    COROSIO_FAULT_REAL(socket, int(*)(int, int, int));
    if(should_fail(sys::socket))
        return -1;
    return real(domain, type, protocol);
}

// glibc marks part of the census __THROW and leaves the rest with no
// exception spec; the shadow has to match exactly or -Werror rejects
// the redeclaration, so there are two macros rather than one. On
// Darwin COROSIO_FAULT_NOTHROW is empty and the two coincide, since no
// entry point there carries a specification to match.
#define COROSIO_FAULT_HOOK(name, ret, failval, params, args)            \
    extern "C" ret name params                                         \
    {                                                                   \
        COROSIO_FAULT_REAL(name, ret(*)params);                         \
        if(should_fail(sys::name))                                      \
            return failval;                                             \
        return real args;                                               \
    }

#define COROSIO_FAULT_HOOK_NX(name, ret, failval, params, args)         \
    extern "C" ret name params COROSIO_FAULT_NOTHROW                    \
    {                                                                   \
        COROSIO_FAULT_REAL(name, ret(*)params);                         \
        if(should_fail(sys::name))                                      \
            return failval;                                             \
        return real args;                                               \
    }

COROSIO_FAULT_HOOK_NX(socketpair, int, -1, (int d, int t, int p, int* sv), (d, t, p, sv))
COROSIO_FAULT_HOOK_NX(bind, int, -1, (int fd, sockaddr const* a, socklen_t l), (fd, a, l))
COROSIO_FAULT_HOOK_NX(listen, int, -1, (int fd, int n), (fd, n))
COROSIO_FAULT_HOOK(accept, int, -1, (int fd, sockaddr* a, socklen_t* l), (fd, a, l))
COROSIO_FAULT_HOOK(connect, int, -1, (int fd, sockaddr const* a, socklen_t l), (fd, a, l))
COROSIO_FAULT_HOOK_NX(getsockname, int, -1, (int fd, sockaddr* a, socklen_t* l), (fd, a, l))
COROSIO_FAULT_HOOK_NX(getpeername, int, -1, (int fd, sockaddr* a, socklen_t* l), (fd, a, l))
COROSIO_FAULT_HOOK_NX(getsockopt, int, -1, (int fd, int lv, int on, void* v, socklen_t* l), (fd, lv, on, v, l))
COROSIO_FAULT_HOOK_NX(setsockopt, int, -1, (int fd, int lv, int on, void const* v, socklen_t l), (fd, lv, on, v, l))
COROSIO_FAULT_HOOK_NX(shutdown, int, -1, (int fd, int how), (fd, how))
COROSIO_FAULT_HOOK(close, int, -1, (int fd), (fd))
COROSIO_FAULT_HOOK(poll, int, -1, (pollfd* p, nfds_t n, int t), (p, n, t))
COROSIO_FAULT_HOOK_NX(pipe, int, -1, (int* p), (p))
COROSIO_FAULT_HOOK_NX(fstat, int, -1, (int fd, struct stat* st), (fd, st))
COROSIO_FAULT_HOOK_NX(lseek, off_t, -1, (int fd, off_t off, int wh), (fd, off, wh))
COROSIO_FAULT_HOOK_NX(ftruncate, int, -1, (int fd, off_t len), (fd, len))
COROSIO_FAULT_HOOK(fsync, int, -1, (int fd), (fd))
COROSIO_FAULT_HOOK_NX(unlink, int, -1, (char const* p), (p))
COROSIO_FAULT_HOOK_NX(sigaction, int, -1, (int sig, struct sigaction const* a, struct sigaction* o), (sig, a, o))
COROSIO_FAULT_HOOK_NX(gethostname, int, -1, (char* n, size_t l), (n, l))

// Linux and FreeBSD both publish these; Darwin has neither, and
// sync_data() lowers to fsync there instead.
#if defined(__linux__) || defined(__FreeBSD__)
COROSIO_FAULT_HOOK(fdatasync, int, -1, (int fd), (fd))

// Return the armed errno directly: this reports failure through the
// return value, not through -1 + errno.
extern "C" int posix_fadvise(int fd, off_t off, off_t len, int advice)
    COROSIO_FAULT_NOTHROW
{
    COROSIO_FAULT_REAL(posix_fadvise, int(*)(int, off_t, off_t, int));
    if(should_fail(sys::posix_fadvise))
        return errno;
    return real(fd, off, len, advice);
}
#endif

#if defined(__linux__)
COROSIO_FAULT_HOOK(accept4, int, -1, (int fd, sockaddr* a, socklen_t* l, int f), (fd, a, l, f))
COROSIO_FAULT_HOOK_NX(epoll_create1, int, -1, (int f), (f))
COROSIO_FAULT_HOOK_NX(epoll_ctl, int, -1, (int ep, int op, int fd, epoll_event* ev), (ep, op, fd, ev))
COROSIO_FAULT_HOOK(epoll_wait, int, -1, (int ep, epoll_event* ev, int n, int t), (ep, ev, n, t))
COROSIO_FAULT_HOOK_NX(eventfd, int, -1, (unsigned v, int f), (v, f))
COROSIO_FAULT_HOOK_NX(timerfd_create, int, -1, (int c, int f), (c, f))
COROSIO_FAULT_HOOK_NX(timerfd_settime, int, -1, (int fd, int f, itimerspec const* n, itimerspec* o), (fd, f, n, o))
#endif

#if defined(__APPLE__) || defined(__FreeBSD__)
COROSIO_FAULT_HOOK(kqueue, int, -1, (), ())
COROSIO_FAULT_HOOK(kevent, int, -1,
    (int kq, struct kevent const* ch, int nch, struct kevent* ev, int nev,
        timespec const* ts),
    (kq, ch, nch, ev, nev, ts))
#endif

#if defined(__APPLE__)
// Darwin's <sys/select.h> spells select as `select$DARWIN_EXTSN` under
// _DARWIN_C_SOURCE and plain `_select` otherwise, and each corosio
// translation unit picks its spelling independently of this one.
// Defining both by asm label shadows the call either way; the plain
// COROSIO_FAULT_HOOK cannot, because the header's own asm label would
// rename it to whichever single spelling this file happens to see.
// The library references the plain spelling today, so the suffixed one
// is a census alias rather than a symbol anything binds to.
extern "C" {
int corosio_fault_select(int, fd_set*, fd_set*, fd_set*, timeval*)
    __asm__("_select");
int corosio_fault_select_extsn(int, fd_set*, fd_set*, fd_set*, timeval*)
    __asm__("_select$DARWIN_EXTSN");
}

namespace {

using select_fn = int(*)(int, fd_set*, fd_set*, fd_set*, timeval*);

// libSystem exports both spellings, but only the plain one is
// guaranteed; fall back so a missing alias cannot leave `real` null.
select_fn real_select(char const* name) noexcept
{
    auto p = reinterpret_cast<select_fn>(::dlsym(RTLD_NEXT, name));
    if(!p)
        p = reinterpret_cast<select_fn>(real_symbol("select"));
    return p;
}

} // namespace

extern "C" int corosio_fault_select(int n, fd_set* r, fd_set* w, fd_set* e,
    timeval* t)
{
    static auto const real = real_select("select");
    if(should_fail(sys::select))
        return -1;
    return real(n, r, w, e, t);
}

extern "C" int corosio_fault_select_extsn(int n, fd_set* r, fd_set* w,
    fd_set* e, timeval* t)
{
    static auto const real = real_select("select$DARWIN_EXTSN");
    if(should_fail(sys::select))
        return -1;
    return real(n, r, w, e, t);
}
#else
COROSIO_FAULT_HOOK(select, int, -1, (int n, fd_set* r, fd_set* w, fd_set* e, timeval* t), (n, r, w, e, t))
#endif

// Both report failure through the return value, not -1 + errno.
extern "C" int getaddrinfo(char const* node, char const* service,
    addrinfo const* hints, addrinfo** res)
{
    COROSIO_FAULT_REAL(getaddrinfo, int(*)(char const*, char const*, addrinfo const*, addrinfo**));
    if(should_fail(sys::getaddrinfo))
        return errno;
    return real(node, service, hints, res);
}

// No failure mode of its own: swallowing the release would leak the
// list the lookup allocated. The arm still counts the call, which is
// what a census of the resolver's teardown path needs.
extern "C" void freeaddrinfo(addrinfo* ai) COROSIO_FAULT_NOTHROW
{
    COROSIO_FAULT_REAL(freeaddrinfo, void(*)(addrinfo*));
    std::ignore = should_fail(sys::freeaddrinfo);
    real(ai);
}

// FreeBSD sizes the host and service buffers with size_t where glibc
// and Darwin spell them socklen_t; the shadow has to match its own
// header exactly or the redeclaration is rejected.
#if defined(__FreeBSD__)
#define COROSIO_FAULT_NI_LEN size_t
#else
#define COROSIO_FAULT_NI_LEN socklen_t
#endif

extern "C" int getnameinfo(sockaddr const* sa, socklen_t salen, char* host,
    COROSIO_FAULT_NI_LEN hostlen, char* serv, COROSIO_FAULT_NI_LEN servlen,
    int flags)
{
    COROSIO_FAULT_REAL(getnameinfo, int(*)(sockaddr const*, socklen_t, char*,
        COROSIO_FAULT_NI_LEN, char*, COROSIO_FAULT_NI_LEN, int));
    if(should_fail(sys::getnameinfo))
        return errno;
    return real(sa, salen, host, hostlen, serv, servlen, flags);
}

extern "C" int fcntl(int fd, int cmd, ...)
{
    COROSIO_FAULT_REAL(fcntl, int(*)(int, int, ...));
    va_list ap;
    va_start(ap, cmd);
    // Every corosio use passes an int or nothing; a long covers both on
    // the SysV and AAPCS64 ABIs.
    long arg = va_arg(ap, long);
    va_end(ap);
    if(should_fail(sys::fcntl))
        return -1;
    return real(fd, cmd, arg);
}

extern "C" int ioctl(int fd, unsigned long req, ...) COROSIO_FAULT_NOTHROW
{
    COROSIO_FAULT_REAL(ioctl, int(*)(int, unsigned long, ...));
    va_list ap;
    va_start(ap, req);
    void* arg = va_arg(ap, void*);
    va_end(ap);
    if(should_fail(sys::ioctl))
        return -1;
    return real(fd, req, arg);
}

extern "C" int open(char const* path, int flags, ...)
{
    COROSIO_FAULT_REAL(open, int(*)(char const*, int, ...));
    unsigned mode = 0;
    if(flags & (O_CREAT | O_TMPFILE))
    {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, unsigned);
        va_end(ap);
    }
    if(should_fail(sys::open))
        return -1;
    return real(path, flags, mode);
}

// The byte-moving census entries additionally consult should_shorten:
// a shortened call still reaches the real function, clamped to the
// armed count, so bytes genuinely move instead of being dropped.
extern "C" ssize_t read(int fd, void* b, size_t n)
{
    COROSIO_FAULT_REAL(read, ssize_t(*)(int, void*, size_t));
    if(should_fail(sys::read))
        return -1;
    std::size_t c;
    if(should_shorten(sys::read, c))
        return c == 0 ? 0 : real(fd, b, c < n ? c : n);
    return real(fd, b, n);
}

extern "C" ssize_t write(int fd, void const* b, size_t n)
{
    COROSIO_FAULT_REAL(write, ssize_t(*)(int, void const*, size_t));
    if(should_fail(sys::write))
        return -1;
    std::size_t c;
    if(should_shorten(sys::write, c))
        return c == 0 ? 0 : real(fd, b, c < n ? c : n);
    return real(fd, b, n);
}

// Defined everywhere though only the kqueue write policy calls it; a
// shadow nothing references costs one forward on the rare libc caller.
extern "C" ssize_t writev(int fd, iovec const* v, int n)
{
    COROSIO_FAULT_REAL(writev, ssize_t(*)(int, iovec const*, int));
    if(should_fail(sys::writev))
        return -1;
    std::size_t c;
    if(should_shorten(sys::writev, c))
    {
        if(c == 0)
            return 0;
        iovec t[64];
        return real(fd, t, truncate_iov(v, n, c, t));
    }
    return real(fd, v, n);
}

extern "C" ssize_t recv(int fd, void* b, size_t n, int f)
{
    COROSIO_FAULT_REAL(recv, ssize_t(*)(int, void*, size_t, int));
    if(should_fail(sys::recv))
        return -1;
    std::size_t c;
    if(should_shorten(sys::recv, c))
        return c == 0 ? 0 : real(fd, b, c < n ? c : n, f);
    return real(fd, b, n, f);
}

extern "C" ssize_t send(int fd, void const* b, size_t n, int f)
{
    COROSIO_FAULT_REAL(send, ssize_t(*)(int, void const*, size_t, int));
    if(should_fail(sys::send))
        return -1;
    std::size_t c;
    if(should_shorten(sys::send, c))
        return c == 0 ? 0 : real(fd, b, c < n ? c : n, f);
    return real(fd, b, n, f);
}

extern "C" ssize_t readv(int fd, iovec const* v, int n)
{
    COROSIO_FAULT_REAL(readv, ssize_t(*)(int, iovec const*, int));
    if(should_fail(sys::readv))
        return -1;
    std::size_t c;
    if(should_shorten(sys::readv, c))
    {
        if(c == 0)
            return 0;
        iovec t[64];
        return real(fd, t, truncate_iov(v, n, c, t));
    }
    return real(fd, v, n);
}

extern "C" ssize_t preadv(int fd, iovec const* v, int n, off_t o)
{
    COROSIO_FAULT_REAL(preadv, ssize_t(*)(int, iovec const*, int, off_t));
    if(should_fail(sys::preadv))
        return -1;
    std::size_t c;
    if(should_shorten(sys::preadv, c))
    {
        if(c == 0)
            return 0;
        iovec t[64];
        return real(fd, t, truncate_iov(v, n, c, t), o);
    }
    return real(fd, v, n, o);
}

extern "C" ssize_t pwritev(int fd, iovec const* v, int n, off_t o)
{
    COROSIO_FAULT_REAL(pwritev, ssize_t(*)(int, iovec const*, int, off_t));
    if(should_fail(sys::pwritev))
        return -1;
    std::size_t c;
    if(should_shorten(sys::pwritev, c))
    {
        if(c == 0)
            return 0;
        iovec t[64];
        return real(fd, t, truncate_iov(v, n, c, t), o);
    }
    return real(fd, v, n, o);
}

extern "C" ssize_t recvmsg(int fd, msghdr* m, int f)
{
    COROSIO_FAULT_REAL(recvmsg, ssize_t(*)(int, msghdr*, int));
    if(should_fail(sys::recvmsg))
        return -1;
    std::size_t c;
    if(should_shorten(sys::recvmsg, c))
    {
        if(c == 0)
            return 0;
        iovec t[64];
        msghdr mh = *m;
        mh.msg_iov = t;
        mh.msg_iovlen = truncate_iov(m->msg_iov, (int)m->msg_iovlen, c, t);
        ssize_t r = real(fd, &mh, f);
        m->msg_namelen = mh.msg_namelen;
        m->msg_flags = mh.msg_flags;
        m->msg_controllen = mh.msg_controllen;
        return r;
    }
    return real(fd, m, f);
}

extern "C" ssize_t sendmsg(int fd, msghdr const* m, int f)
{
    COROSIO_FAULT_REAL(sendmsg, ssize_t(*)(int, msghdr const*, int));
    if(should_fail(sys::sendmsg))
        return -1;
    std::size_t c;
    if(should_shorten(sys::sendmsg, c))
    {
        if(c == 0)
            return 0;
        iovec t[64];
        msghdr mh = *m;
        mh.msg_iov = t;
        mh.msg_iovlen = truncate_iov(m->msg_iov, (int)m->msg_iovlen, c, t);
        return real(fd, &mh, f);
    }
    return real(fd, m, f);
}

#if defined(__linux__)
// _FORTIFY_SOURCE routes these through the *_chk entry points, which
// would otherwise reach libc's read/recv/poll directly.
extern "C" ssize_t __read_chk(int fd, void* b, size_t n, size_t)
{
    return ::read(fd, b, n);
}

extern "C" ssize_t __recv_chk(int fd, void* b, size_t n, size_t, int f)
{
    return ::recv(fd, b, n, f);
}

extern "C" ssize_t __recvfrom_chk(int fd, void* b, size_t n, size_t, int f,
    sockaddr* a, socklen_t* l)
{
    COROSIO_FAULT_REAL(recvfrom, ssize_t(*)(int, void*, size_t, int, sockaddr*, socklen_t*));
    if(should_fail(sys::recv))
        return -1;
    return real(fd, b, n, f, a, l);
}

extern "C" int __poll_chk(pollfd* p, nfds_t n, int t, size_t)
{
    return ::poll(p, n, t);
}

extern "C" ssize_t __pread64_chk(int fd, void* b, size_t n, off64_t o, size_t)
{
    COROSIO_FAULT_REAL(pread64, ssize_t(*)(int, void*, size_t, off64_t));
    if(should_fail(sys::read))
        return -1;
    return real(fd, b, n, o);
}

// __open_2 is called with the mode omitted, but our `open` shadow is
// variadic and reads a mode via va_arg when O_CREAT/O_TMPFILE is set;
// passing an explicit dummy 0 keeps that read well-defined instead of
// pulling an unsupplied argument. glibc's own __open_2 aborts if
// O_CREAT is set without a mode — no corosio call site does that.
extern "C" int __open_2(char const* path, int flags)
{
    return ::open(path, flags, 0);
}

extern "C" int __gethostname_chk(char* b, size_t n, size_t) noexcept
{
    return ::gethostname(b, n);
}
#endif

namespace boost::corosio::test::fault {

#if defined(__APPLE__)
namespace {

// dyld's index for the loaded corosio dylib, or -1 in a static build.
// Taking the address of a dylib function from the executable can yield
// a stub in this image, so dladdr reports both addresses in the same
// image and cannot tell a shared build from a static one; ask dyld
// directly instead. Index 0 is the executable, and only a leading
// basename counts: a static build whose own path happens to contain
// the library name must not read as shared.
int corosio_image_index() noexcept
{
    static constexpr char prefix[] = "libboost_corosio";
    for(std::uint32_t i = 1, n = ::_dyld_image_count(); i < n; ++i)
    {
        char const* path = ::_dyld_get_image_name(i);
        if(!path)
            continue;
        char const* slash = std::strrchr(path, '/');
        char const* base = slash ? slash + 1 : path;
        if(std::strncmp(base, prefix, sizeof(prefix) - 1) == 0)
            return static_cast<int>(i);
    }
    return -1;
}

} // namespace
#endif

bool corosio_is_shared() noexcept
{
#if defined(__APPLE__)
    return corosio_image_index() >= 0;
#else
    Dl_info lib{}, exe{};
    // host_name is an ordinary exported corosio function; the hook
    // lives in the executable by construction.
    ::dladdr(reinterpret_cast<void const*>(&boost::corosio::host_name), &lib);
    ::dladdr(reinterpret_cast<void const*>(&::socket), &exe);
    return lib.dli_fbase != exe.dli_fbase;
#endif
}

namespace {

struct census_entry
{
    char const* name;
    void const* hook;
};

#define COROSIO_FAULT_CENSUS(name) { #name, reinterpret_cast<void const*>(&::name) }

} // namespace

#if defined(__linux__)
// Not declared by any header we include in a non-fortified build (glibc
// only exposes these under __USE_FORTIFY_LEVEL > 0); the definitions
// above are the only declaration these need.
extern "C" ssize_t __read_chk(int, void*, size_t, size_t);
extern "C" ssize_t __recv_chk(int, void*, size_t, size_t, int);
extern "C" ssize_t __recvfrom_chk(int, void*, size_t, size_t, int, sockaddr*, socklen_t*);
extern "C" int __poll_chk(pollfd*, nfds_t, int, size_t);
extern "C" ssize_t __pread64_chk(int, void*, size_t, off64_t, size_t);
extern "C" int __open_2(char const*, int);
extern "C" int __gethostname_chk(char*, size_t, size_t) noexcept;
#endif

namespace {

// One entry per OS symbol the library is expected to reference on this
// platform, read off `nm -u` of the built library. It is the ledger the
// shared-build readback checks, so a symbol no backend here calls does
// not belong in it even when the shadow exists: the Darwin build never
// seeks with lseek, and the epoll/timerfd/eventfd family and the glibc
// fortify aliases have no Darwin counterpart at all.
[[maybe_unused]] census_entry const census[] = {
    COROSIO_FAULT_CENSUS(socket), COROSIO_FAULT_CENSUS(socketpair),
    COROSIO_FAULT_CENSUS(bind), COROSIO_FAULT_CENSUS(listen),
    COROSIO_FAULT_CENSUS(accept),
    COROSIO_FAULT_CENSUS(connect), COROSIO_FAULT_CENSUS(getsockname),
    COROSIO_FAULT_CENSUS(getpeername), COROSIO_FAULT_CENSUS(getsockopt),
    COROSIO_FAULT_CENSUS(setsockopt), COROSIO_FAULT_CENSUS(shutdown),
    COROSIO_FAULT_CENSUS(close), COROSIO_FAULT_CENSUS(read),
    COROSIO_FAULT_CENSUS(write), COROSIO_FAULT_CENSUS(readv),
    COROSIO_FAULT_CENSUS(preadv), COROSIO_FAULT_CENSUS(pwritev),
    COROSIO_FAULT_CENSUS(recv), COROSIO_FAULT_CENSUS(send),
    COROSIO_FAULT_CENSUS(recvmsg), COROSIO_FAULT_CENSUS(sendmsg),
    COROSIO_FAULT_CENSUS(poll), COROSIO_FAULT_CENSUS(pipe),
    COROSIO_FAULT_CENSUS(fcntl), COROSIO_FAULT_CENSUS(ioctl),
    COROSIO_FAULT_CENSUS(open), COROSIO_FAULT_CENSUS(fstat),
    COROSIO_FAULT_CENSUS(ftruncate),
    COROSIO_FAULT_CENSUS(fsync), COROSIO_FAULT_CENSUS(unlink),
    COROSIO_FAULT_CENSUS(sigaction), COROSIO_FAULT_CENSUS(getaddrinfo),
    COROSIO_FAULT_CENSUS(freeaddrinfo),
    COROSIO_FAULT_CENSUS(getnameinfo), COROSIO_FAULT_CENSUS(gethostname),
#if defined(__linux__) || defined(__FreeBSD__)
    COROSIO_FAULT_CENSUS(fdatasync), COROSIO_FAULT_CENSUS(posix_fadvise),
#endif
    // Darwin spells select with an asm label and gets its two aliases
    // below instead of the plain name.
#if !defined(__APPLE__)
    COROSIO_FAULT_CENSUS(select),
#endif
#if defined(__linux__)
    COROSIO_FAULT_CENSUS(accept4), COROSIO_FAULT_CENSUS(lseek),
    COROSIO_FAULT_CENSUS(epoll_create1), COROSIO_FAULT_CENSUS(epoll_ctl),
    COROSIO_FAULT_CENSUS(epoll_wait), COROSIO_FAULT_CENSUS(eventfd),
    COROSIO_FAULT_CENSUS(timerfd_create), COROSIO_FAULT_CENSUS(timerfd_settime),
    COROSIO_FAULT_CENSUS(__read_chk), COROSIO_FAULT_CENSUS(__recv_chk),
    COROSIO_FAULT_CENSUS(__recvfrom_chk), COROSIO_FAULT_CENSUS(__poll_chk),
    COROSIO_FAULT_CENSUS(__pread64_chk),
    COROSIO_FAULT_CENSUS(__open_2), COROSIO_FAULT_CENSUS(__gethostname_chk),
#endif
#if defined(__APPLE__) || defined(__FreeBSD__)
    COROSIO_FAULT_CENSUS(writev), COROSIO_FAULT_CENSUS(kqueue),
    COROSIO_FAULT_CENSUS(kevent),
#endif
#if defined(__APPLE__)
    { "select", reinterpret_cast<void const*>(&::corosio_fault_select) },
    { "select$DARWIN_EXTSN",
        reinterpret_cast<void const*>(&::corosio_fault_select_extsn) },
#endif
#if BOOST_COROSIO_HAVE_LIBURING
    COROSIO_FAULT_CENSUS(io_uring_queue_init_params),
    COROSIO_FAULT_CENSUS(io_uring_queue_exit),
    COROSIO_FAULT_CENSUS(io_uring_submit),
    COROSIO_FAULT_CENSUS(io_uring_submit_and_wait_timeout),
    COROSIO_FAULT_CENSUS(io_uring_submit_and_get_events),
    COROSIO_FAULT_CENSUS(io_uring_wait_cqe_timeout),
#endif
};

// Alias entries name a second spelling of a symbol the library may or
// may not have been built to call: the glibc fortify wrappers and the
// Darwin `$` suffixes.
[[maybe_unused]] bool is_alias_entry(char const* name) noexcept
{
    return std::strncmp(name, "__", 2) == 0 || std::strchr(name, '$');
}

#if defined(__APPLE__)

// One census symbol whose import slot in the dylib is to be rewritten.
// Mach-O's two-level namespace records libSystem as the source of the
// dylib's imports at link time, so a bound slot holds exactly
// libSystem's entry point for the symbol and can be recognised by its
// value alone. That reads the same in a classic lazy-pointer section
// and in the `__got` of a chained-fixups image, and needs neither the
// indirect symbol table nor the chained-fixup imports table, both of
// which vary with the linker that produced the dylib.
struct rebind_target
{
    char const* name;
    void const* hook;
    void const* real;
    unsigned rebound;
    unsigned unbound;
};

// Visit every section of `hdr` that can hold a bound import pointer:
// the classic lazy and non-lazy pointer sections, plus the `__got`
// family a chained-fixups image uses in their place.
template<class F>
void for_each_import_section(mach_header_64 const* hdr, std::intptr_t slide,
    F&& f) noexcept
{
    auto const* cmd = reinterpret_cast<load_command const*>(hdr + 1);
    for(std::uint32_t i = 0; i < hdr->ncmds; ++i)
    {
        if(cmd->cmd == LC_SEGMENT_64)
        {
            auto const* seg = reinterpret_cast<segment_command_64 const*>(cmd);
            auto const* sec = reinterpret_cast<section_64 const*>(seg + 1);
            for(std::uint32_t j = 0; j < seg->nsects; ++j, ++sec)
            {
                std::uint32_t const type = sec->flags & SECTION_TYPE;
                if(type != S_NON_LAZY_SYMBOL_POINTERS &&
                    type != S_LAZY_SYMBOL_POINTERS &&
                    std::strncmp(sec->sectname, "__got", 16) != 0 &&
                    std::strncmp(sec->sectname, "__auth_got", 16) != 0)
                    continue;
                std::size_t const count =
                    static_cast<std::size_t>(sec->size) / sizeof(void*);
                if(count == 0)
                    continue;
                f(seg, sec, reinterpret_cast<void**>(
                    static_cast<std::uintptr_t>(sec->addr) + slide), count);
            }
        }
        cmd = reinterpret_cast<load_command const*>(
            reinterpret_cast<char const*>(cmd) + cmd->cmdsize);
    }
}

// Ask the kernel for `prot` over the pages covering the byte range.
// VM_PROT_COPY asks for a private copy of a file-backed mapping, which
// is what a plain write request is refused on.
bool protect_pages(void* addr, std::size_t bytes, vm_prot_t prot) noexcept
{
    auto const page = static_cast<vm_address_t>(::getpagesize());
    auto const start = reinterpret_cast<vm_address_t>(addr);
    vm_address_t const begin = start & ~(page - 1);
    vm_size_t const len = ((start + bytes + page - 1) & ~(page - 1)) - begin;
    if(::vm_protect(mach_task_self(), begin, len, FALSE, prot)
        == KERN_SUCCESS)
        return true;
    return ::vm_protect(mach_task_self(), begin, len, FALSE,
        prot | VM_PROT_COPY) == KERN_SUCCESS;
}

// True for a segment dyld maps read-only again once it has applied its
// fixups, which is the one that has to be reprotected after the write.
bool is_const_segment(segment_command_64 const* seg) noexcept
{
    char name[17] = {};
    std::memcpy(name, seg->segname, 16);
    return std::strstr(name, "_CONST") != nullptr;
}

// Point every matching import slot at the shadow. Reports through
// `ok`, and leaves the counting to the verification pass so that what
// is checked is the memory as it stands afterwards, not what this
// pass believes it wrote.
void rebind_imports(mach_header_64 const* hdr, std::intptr_t slide,
    rebind_target const* targets, std::size_t n, bool& ok) noexcept
{
    for_each_import_section(hdr, slide,
        [&](segment_command_64 const* seg, section_64 const* sec,
            void** slots, std::size_t count)
        {
            std::size_t hits = 0;
            for(std::size_t i = 0; i < count; ++i)
            {
                for(std::size_t k = 0; k < n; ++k)
                {
                    if(slots[i] == targets[k].real)
                        ++hits;
                }
            }
            if(hits == 0)
                return;
            if(!protect_pages(slots, count * sizeof(void*),
                VM_PROT_READ | VM_PROT_WRITE))
            {
                std::fprintf(stderr,
                    "fault harness: %.16s,%.16s refused to become writable\n",
                    seg->segname, sec->sectname);
                ok = false;
                return;
            }
            for(std::size_t i = 0; i < count; ++i)
            {
                for(std::size_t k = 0; k < n; ++k)
                {
                    if(slots[i] != targets[k].real)
                        continue;
                    slots[i] = const_cast<void*>(targets[k].hook);
                    break;
                }
            }
            if(is_const_segment(seg))
                std::ignore = protect_pages(slots, count * sizeof(void*),
                    VM_PROT_READ);
        });
}

// Count, per symbol, the slots that now hold the shadow and the slots
// that still hold libSystem's entry point. `scanned` carries the size
// of the search, which is what separates a rewrite that missed a
// symbol from a Mach-O layout this walk does not recognise at all.
void tally_imports(mach_header_64 const* hdr, std::intptr_t slide,
    rebind_target* targets, std::size_t n, std::size_t& sections,
    std::size_t& scanned) noexcept
{
    for_each_import_section(hdr, slide,
        [&](segment_command_64 const*, section_64 const*, void** slots,
            std::size_t count)
        {
            ++sections;
            scanned += count;
            for(std::size_t i = 0; i < count; ++i)
            {
                for(std::size_t k = 0; k < n; ++k)
                {
                    if(slots[i] == targets[k].hook)
                        ++targets[k].rebound;
                    else if(slots[i] == targets[k].real)
                        ++targets[k].unbound;
                }
            }
        });
}

// Rewrite the loaded corosio dylib's import slots so that library code
// reaches the shadows. dyld applies `__DATA,__interpose` only from
// dylibs, never from the main executable, and the two-level namespace
// leaves an executable-defined shadow out of the search entirely, so
// this is the only interposition available to a test binary.
void interpose_corosio_dylib() noexcept
{
#if defined(__has_feature)
#if __has_feature(ptrauth_calls)
    // An arm64e slot holds a signed pointer; a plain store would
    // install a value the caller's authenticated branch rejects.
    die("fault harness: pointer-authenticated import slots cannot be "
        "rebound by a plain store");
#endif
#endif
    int const image = corosio_image_index();
    if(image < 0)
        die("fault harness: the corosio dylib left dyld's image list");
    auto const* hdr = reinterpret_cast<mach_header_64 const*>(
        ::_dyld_get_image_header(static_cast<std::uint32_t>(image)));
    if(!hdr || hdr->magic != MH_MAGIC_64)
        die("fault harness: the corosio dylib is not a 64-bit Mach-O image");
    auto const slide =
        ::_dyld_get_image_vmaddr_slide(static_cast<std::uint32_t>(image));

    rebind_target targets[sizeof(census) / sizeof(census[0])] = {};
    std::size_t n = 0;
    for(auto const& e : census)
    {
        // An alias is a second spelling the library is not known to
        // bind; libSystem may even give both spellings one entry
        // point, which a value match cannot tell apart.
        if(is_alias_entry(e.name))
            continue;
        void* real = ::dlsym(RTLD_NEXT, e.name);
        if(!real)
        {
            char msg[160];
            std::snprintf(msg, sizeof(msg),
                "fault harness: %s has no implementation behind the shadow",
                e.name);
            die(msg);
        }
        for(std::size_t k = 0; k < n; ++k)
        {
            if(targets[k].real != real)
                continue;
            char msg[192];
            std::snprintf(msg, sizeof(msg),
                "fault harness: %s and %s share one libSystem entry point",
                targets[k].name, e.name);
            die(msg);
        }
        targets[n].name = e.name;
        targets[n].hook = e.hook;
        targets[n].real = real;
        ++n;
    }

    bool ok = true;
    rebind_imports(hdr, slide, targets, n, ok);
    std::size_t sections = 0, scanned = 0;
    tally_imports(hdr, slide, targets, n, sections, scanned);
    for(std::size_t k = 0; k < n; ++k)
    {
        auto const& t = targets[k];
        if(t.unbound != 0)
        {
            std::fprintf(stderr, "fault harness: %s still reaches libSystem "
                "through %u of the dylib's import slots\n", t.name, t.unbound);
            ok = false;
        }
        // A census name with no slot at all is as much a defect as one
        // that refused to move: every arm on it would sit dead.
        if(t.rebound == 0)
        {
            std::fprintf(stderr,
                "fault harness: %s is not among the dylib's imports\n",
                t.name);
            ok = false;
        }
    }
    if(!ok)
    {
        std::fprintf(stderr, "fault harness: %zu import sections, %zu slots "
            "scanned in %s\n", sections, scanned,
            ::_dyld_get_image_name(static_cast<std::uint32_t>(image)));
        die("fault harness: the corosio dylib's imports were not rebound");
    }
}
#endif

// A shared build only reaches the shadows through the dynamic loader,
// and a loader that resolved the library's calls elsewhere would leave
// every fault silently dead. Prove the binding instead of assuming it:
// on ELF the library binds through the executable's dynamic symbol
// table, which -Bsymbolic or -fno-plt would bypass; on Mach-O the
// binding has to be installed here first.
int const readback = []
{
    if(!corosio_is_shared())
        return 0;
#if defined(__APPLE__)
    interpose_corosio_dylib();
#else
    bool ok = true;
    for(auto const& e : census)
    {
        void* bound = ::dlsym(RTLD_DEFAULT, e.name);
        // The linker exports an executable symbol into .dynsym only
        // when a linked .so references it; a mismatch here just means
        // the library wasn't built to call this alias. A genuinely
        // broken interposition (-Bsymbolic, -fno-plt) fails on the
        // plain census names first, not here.
        if(bound != e.hook && is_alias_entry(e.name))
            continue;
        if(bound != e.hook)
        {
            std::fprintf(stderr, "fault harness: %s is bound to %p, hook is %p\n",
                e.name, bound, e.hook);
            ok = false;
        }
    }
    if(!ok)
        die("fault harness: shadows are not interposing libboost_corosio.so");
#endif
    return 0;
}();

} // namespace
} // boost::corosio::test::fault
