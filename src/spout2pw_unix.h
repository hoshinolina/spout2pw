#include <stdint.h>

#define __WINESRC__

#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winuser.h>
#include <ntuser.h>
#include "wine/unixlib.h"

// Wine 9.0 build compat (Build only! This will not work with wine 9.0 at runtime!)
#ifndef ALL_USER32_CALLBACKS
#define NtUserCallDispatchCallback 0

/* NtUserCallDispatchCallback params */
struct dispatch_callback_params
{
    UINT64 callback;
};

NTSYSAPI NTSTATUS KeUserModeCallback( ULONG id, const void *args, ULONG len, void **ret_ptr, ULONG *ret_len );

static inline NTSTATUS KeUserDispatchCallback( const struct dispatch_callback_params *params, ULONG len,
                                               void **ret_ptr, ULONG *ret_len )
{
    if (!params->callback) return STATUS_ENTRYPOINT_NOT_FOUND;
    return KeUserModeCallback( NtUserCallDispatchCallback, params, len, ret_ptr, ret_len );
}

#endif

struct receiver_params
{
    struct dispatch_callback_params dispatch;
    void *receiver;
};

#define RECEIVER_DISCONNECTED (1<<0)
#define RECEIVER_TEXTURE_UPDATED (1<<1)
#define RECEIVER_TEXTURE_INVALID (1<<2)

struct source_info
{
    uint64_t resource_size;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t usage;
    uint32_t bind_flags;
    int32_t opaque_fd;
};

#define FRAME_IS_NEW (1<<0)

struct lock_texture_return
{
    struct dispatch_callback_params dispatch;
    int32_t retval;
    uint32_t flags;
    uint64_t frame_count;
};

struct startup_params
{
    UINT64 lock_texture;
    UINT64 unlock_texture;
};

struct create_source_params
{
    const char *sender_name;
    void *receiver;
    struct source_info info;

    // return
    void *ret_source;
};

struct update_source_params
{
    void *source;
    struct source_info info;
};

enum spout2xdp_funcs
{
    unix_libs_init,
    unix_startup,
    unix_teardown,
    unix_create_source,
    unix_run_source,
    unix_update_source,
    unix_destroy_source,
    unix_funcs_count
};

#define UNIX_CALL( func, params ) WINE_UNIX_CALL( unix_ ## func, params )
