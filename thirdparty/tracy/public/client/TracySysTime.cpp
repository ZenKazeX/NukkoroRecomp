#include "TracySysTime.hpp"

#ifdef TRACY_HAS_SYSTIME

#  if defined _WIN32
#    include <windows.h>
#    include "../common/TracyWinFamily.hpp"
#  elif defined __linux__
#    include <stdio.h>
#    include <inttypes.h>
#  elif defined __APPLE__
#    include <mach/mach_host.h>
#    include <mach/host_info.h>
#  elif defined __FreeBSD__ || defined __NetBSD__ || defined __OpenBSD__ || defined __DragonFly__
#    include <sys/types.h>
#    include <sys/sysctl.h>
#  endif

namespace tracy
{

#  if defined _WIN32

static inline uint64_t ConvertTime( const FILETIME& t )
{
    return ( uint64_t( t.dwHighDateTime ) << 32 ) | uint64_t( t.dwLowDateTime );
}

void SysTime::ReadTimes()
{
    FILETIME kernelTime;
    FILETIME userTime;

#    if defined TRACY_SYSTIME_PROCESS_ONLY
    // GEKKOFORGE LOCAL PATCH - see GekkoForge/docs/TRACY_PROFILING.md, and
    // re-apply it after any Tracy upgrade.
    //
    // Upstream reads GetSystemTimes(): every process on the machine. The
    // viewer's "CPU usage" plot therefore mixed the game with whatever else was
    // running, which is not what a profile of the game should show. This reads
    // THIS process only, expressed as a share of the whole machine
    // (100% = every logical processor busy with this process), so Get()'s
    // formula and the plot's scale are unchanged:
    //
    //   used = logical processors x wall time since the process started
    //   idle = used - this process's kernel + user time
    //
    // making (used - idle) / used this process's share of the capacity.
    // Anchoring at the creation time keeps the product small: 100 ns units
    // since 1601 times a large processor count would overflow 64 bits.
    FILETIME creationTime;
    FILETIME exitTime;
    FILETIME nowTime;
    GetProcessTimes( GetCurrentProcess(), &creationTime, &exitTime, &kernelTime, &userTime );
    GetSystemTimeAsFileTime( &nowTime );

    const uint64_t start = ConvertTime( creationTime );
    const uint64_t now = ConvertTime( nowTime );
    const DWORD processors = GetActiveProcessorCount( ALL_PROCESSOR_GROUPS );
    const uint64_t capacity = ( now > start ? now - start : 0 ) * ( processors != 0 ? processors : 1 );
    const uint64_t busy = ConvertTime( kernelTime ) + ConvertTime( userTime );

    used = capacity;
    idle = capacity > busy ? capacity - busy : 0;
    return;
#    elif defined TRACY_GDK
    FILETIME creationTime;
    FILETIME exitTime;

    GetProcessTimes( GetCurrentProcess(), &creationTime, &exitTime, &kernelTime, &userTime );

    idle = 0;
#    else
    FILETIME idleTime;

    GetSystemTimes( &idleTime, &kernelTime, &userTime );

    idle = ConvertTime( idleTime );
#    endif

    const auto kernel = ConvertTime( kernelTime );
    const auto user = ConvertTime( userTime );
    used = kernel + user;
}

#  elif defined __linux__

void SysTime::ReadTimes()
{
    uint64_t user, nice, system;
    FILE* f = fopen( "/proc/stat", "r" );
    if( f )
    {
        int read = fscanf( f, "cpu %" PRIu64 " %" PRIu64 " %" PRIu64" %" PRIu64, &user, &nice, &system, &idle );
        fclose( f );
        if (read == 4)
        {
            used = user + nice + system;
        }
    }
}

#  elif defined __APPLE__

void SysTime::ReadTimes()
{
    host_cpu_load_info_data_t info;
    mach_msg_type_number_t cnt = HOST_CPU_LOAD_INFO_COUNT;
    host_statistics( mach_host_self(), HOST_CPU_LOAD_INFO, reinterpret_cast<host_info_t>( &info ), &cnt );
    used = info.cpu_ticks[CPU_STATE_USER] + info.cpu_ticks[CPU_STATE_NICE] + info.cpu_ticks[CPU_STATE_SYSTEM];
    idle = info.cpu_ticks[CPU_STATE_IDLE];
}

#  elif defined __FreeBSD__ || defined __NetBSD__ || defined __OpenBSD__ || defined __DragonFly__

void SysTime::ReadTimes()
{
    u_long data[5];
    size_t sz = sizeof( data );
    sysctlbyname( "kern.cp_time", &data, &sz, nullptr, 0 );
    used = data[0] + data[1] + data[2] + data[3];
    idle = data[4];
}

#endif

SysTime::SysTime()
{
    ReadTimes();
}

float SysTime::Get()
{
    const auto oldUsed = used;
    const auto oldIdle = idle;

    ReadTimes();

    const auto diffIdle = idle - oldIdle;
    const auto diffUsed = used - oldUsed;

#if defined _WIN32 && defined TRACY_SYSTIME_PROCESS_ONLY
    // GEKKOFORGE LOCAL PATCH (see ReadTimes). With a wall-clock capacity, the
    // process can be accounted a scheduler tick more CPU than the interval's
    // capacity, so idle can step BACKWARDS. The unsigned subtraction upstream
    // relies on idle being monotonic and would wrap into an absurd spike; do
    // it signed and clamp to the plot's 0..100 range instead.
    if( diffUsed == 0 ) return -1;
    const float share = float( int64_t( diffUsed ) - int64_t( diffIdle ) ) * 100.f / float( diffUsed );
    return share < 0.f ? 0.f : ( share > 100.f ? 100.f : share );
#elif defined _WIN32
    return diffUsed == 0 ? -1 : ( diffUsed - diffIdle ) * 100.f / diffUsed;
#elif defined __linux__ || defined __APPLE__ || defined __FreeBSD__ || defined __NetBSD__ || defined __OpenBSD__ || defined __DragonFly__
    const auto total = diffUsed + diffIdle;
    return total == 0 ? -1 : diffUsed * 100.f / total;
#endif
}

}

#endif
