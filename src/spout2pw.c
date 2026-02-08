#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "spout2pw_unix.h"

#include <winbase.h>
#include <windef.h>
#include <winnt.h>
#include <winsvc.h>

#include <winioctl.h>

#include "wine/debug.h"
#include "wine/server.h"

#include <spoutdxtoc.h>

WINE_DEFAULT_DEBUG_CHANNEL(spout2pw);

static WCHAR spout2pwW[] = L"Spout2Pw";
static HANDLE exit_event;
static SERVICE_STATUS_HANDLE service_handle;
static SERVICE_STATUS service_status;

static HANDLE sendernames_thread_handle = 0;
static SPOUTDXTOC_SENDERNAMES *spout_names = NULL;

static DWORD WINAPI sendernames_thread(void *arg);

static bool do_restart = false;

#define IOCTL_SHARED_GPU_RESOURCE_GET_UNIX_RESOURCE                            \
    CTL_CODE(FILE_DEVICE_VIDEO, 3, METHOD_BUFFERED, FILE_READ_ACCESS)

#define IOCTL_SHARED_GPU_RESOURCE_OPEN                                         \
    CTL_CODE(FILE_DEVICE_VIDEO, 1, METHOD_BUFFERED, FILE_WRITE_ACCESS)

#define IOCTL_SHARED_GPU_RESOURCE_GET_METADATA                                 \
    CTL_CODE(FILE_DEVICE_VIDEO, 5, METHOD_BUFFERED, FILE_READ_ACCESS)

#define IOCTL_SHARED_GPU_RESOURCE_GET_INFO                                     \
    CTL_CODE(FILE_DEVICE_VIDEO, 7, METHOD_BUFFERED, FILE_READ_ACCESS)

struct receiver {
    char *name;
    void *source;
    SPOUTDXTOC_RECEIVER *spout;
    HANDLE thread;
    struct source_info info;
    bool force_update;
};

struct receiver **receivers;
size_t num_receivers = 0;

struct shared_resource_open {
    unsigned int kmt_handle;
    WCHAR name[1];
};

struct shared_resource_info {
    UINT64 resource_size;
};

typedef enum D3D11_TEXTURE_LAYOUT {
    D3D11_TEXTURE_LAYOUT_UNDEFINED = 0,
    D3D11_TEXTURE_LAYOUT_ROW_MAJOR = 1,
    D3D11_TEXTURE_LAYOUT_64K_STANDARD_SWIZZLE = 2
} D3D11_TEXTURE_LAYOUT;

struct DxvkSharedTextureMetadata {
    UINT Width;
    UINT Height;
    UINT MipLevels;
    UINT ArraySize;
    DXGI_FORMAT Format;
    DXGI_SAMPLE_DESC SampleDesc;
    D3D11_USAGE Usage;
    UINT BindFlags;
    UINT CPUAccessFlags;
    UINT MiscFlags;
    D3D11_TEXTURE_LAYOUT TextureLayout;
};

static inline void init_unicode_string(UNICODE_STRING *str, const WCHAR *data) {
    str->Length = wcslen(data) * sizeof(WCHAR);
    str->MaximumLength = str->Length + sizeof(WCHAR);
    str->Buffer = (WCHAR *)data;
}

static HANDLE open_shared_resource(HANDLE kmt_handle) {
    static const WCHAR shared_gpu_resourceW[] = {
        '\\', '?', '?', '\\', 'S', 'h', 'a', 'r', 'e', 'd', 'G',
        'p',  'u', 'R', 'e',  's', 'o', 'u', 'r', 'c', 'e', 0};
    UNICODE_STRING shared_gpu_resource_us;
    struct shared_resource_open *inbuff;
    HANDLE shared_resource;
    OBJECT_ATTRIBUTES attr;
    IO_STATUS_BLOCK iosb;
    NTSTATUS status;
    DWORD in_size;

    init_unicode_string(&shared_gpu_resource_us, shared_gpu_resourceW);

    attr.Length = sizeof(attr);
    attr.RootDirectory = 0;
    attr.Attributes = 0;
    attr.ObjectName = &shared_gpu_resource_us;
    attr.SecurityDescriptor = NULL;
    attr.SecurityQualityOfService = NULL;

    if ((status = NtCreateFile(&shared_resource, GENERIC_READ | GENERIC_WRITE,
                               &attr, &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, 0,
                               NULL, 0))) {
        ERR("Failed to load open a shared resource handle, status %#lx.\n",
            (long int)status);
        return INVALID_HANDLE_VALUE;
    }

    in_size = sizeof(*inbuff);
    inbuff = calloc(1, in_size);
    inbuff->kmt_handle = wine_server_obj_handle(kmt_handle);

    status = NtDeviceIoControlFile(shared_resource, NULL, NULL, NULL, &iosb,
                                   IOCTL_SHARED_GPU_RESOURCE_OPEN, inbuff,
                                   in_size, NULL, 0);

    free(inbuff);

    if (status) {
        ERR("Failed to open video resource, status %#lx.\n", (long int)status);
        NtClose(shared_resource);
        return INVALID_HANDLE_VALUE;
    }

    return shared_resource;
}

static NTSTATUS get_shared_metadata(HANDLE handle, void *buf, uint32_t buf_size,
                                    uint32_t *metadata_size) {
    IO_STATUS_BLOCK iosb;

    NTSTATUS status = NtDeviceIoControlFile(
        handle, NULL, NULL, NULL, &iosb, IOCTL_SHARED_GPU_RESOURCE_GET_METADATA,
        NULL, 0, buf, buf_size);

    if (status != STATUS_SUCCESS) {
        ERR("Failed to get shared metadata, status %#lx.\n", (long int)status);
    } else if (metadata_size) {
        *metadata_size = iosb.Information;
    }
    return status;
}

static NTSTATUS get_shared_info(HANDLE handle,
                                struct shared_resource_info *info) {
    IO_STATUS_BLOCK iosb;

    NTSTATUS status = NtDeviceIoControlFile(handle, NULL, NULL, NULL, &iosb,
                                            IOCTL_SHARED_GPU_RESOURCE_GET_INFO,
                                            NULL, 0, info, sizeof(*info));

    if (status != STATUS_SUCCESS) {
        ERR("Failed to get shared info, status %#lx.\n", (long int)status);
    }
    return status;
}

static NTSTATUS WINAPI lock_texture(void *args, ULONG size) {
    struct receiver_params *params = args;
    struct receiver *receiver = params->receiver;
    SPOUTDXTOC_RECEIVER *recv = receiver->spout;
    struct lock_texture_return ret = {.retval = 0};

    if (!SpoutDXToCCheckTextureAccess(recv)) {
        ERR("Failed to lock shared texture\n");
        ret.retval = -1;
    } else {
        if (SpoutDXToCGetFrameCount(recv, &ret.frame_count)) {
            ret.flags |= FRAME_IS_NEW;
        }
    }

    return NtCallbackReturn(&ret, sizeof(ret), STATUS_SUCCESS);
}

static NTSTATUS WINAPI unlock_texture(void *args, ULONG size) {
    struct receiver_params *params = args;
    struct receiver *receiver = params->receiver;
    SPOUTDXTOC_RECEIVER *spout = receiver->spout;

    SpoutDXToCAllowTextureAccess(spout);

    return NtCallbackReturn(NULL, 0, STATUS_SUCCESS);
}

static void trigger_restart(void) {
    TRACE("Restarting service due to error\n");
    do_restart = true;
    SetEvent(exit_event);
}

static DWORD WINAPI receiver_thread(void *arg) {
    struct receiver *receiver = arg;

    TRACE("Receiver thread starting for %s\n", receiver->name);
    UNIX_CALL(run_source, receiver->source);
    TRACE("Receiver thread exiting for %s\n", receiver->name);

    return STATUS_SUCCESS;
}

static struct source_info get_receiver_info(struct receiver *receiver) {
    SPOUTDXTOC_RECEIVER *spout = receiver->spout;
    SPOUTDXTOC_SENDERINFO info;
    struct source_info ret = {.opaque_fd = -1};

    TRACE("Updating receiver %p -> %p (%s)\n", receiver, spout, receiver->name);

    if (!SpoutDXToCIsConnected(spout)) {
        ret.flags = RECEIVER_DISCONNECTED;
        TRACE("-> Not connected\n");
        return ret;
    }

    if (!SpoutDXToCGetSenderInfo(spout, &info)) {
        ret.flags = RECEIVER_DISCONNECTED;
        TRACE("-> Failed to get sender info (disconnected?)\n");
        return ret;
    }

    HANDLE share_handle = info.shareHandle;

    TRACE("Sender %dx%d fmt=%d handle=0x%lx usage=0x%x changed=%d\n",
          info.width, info.height, info.format,
          (long)(intptr_t)info.shareHandle, info.usage, info.changed);

    ret.width = info.width;
    ret.height = info.height;
    ret.format = info.format;
    ret.usage = info.usage;

    if (!info.changed && !receiver->force_update)
        return ret;

    receiver->force_update = true;

    int fd;
    NTSTATUS status;
    IO_STATUS_BLOCK iosb;
    obj_handle_t unix_resource;
    HANDLE memhandle = open_shared_resource(info.shareHandle);
    if (memhandle == INVALID_HANDLE_VALUE) {
        ret.flags |= RECEIVER_TEXTURE_INVALID;
        WARN("Share handle open failed\n");
        return ret;
    }

    TRACE("Share handle opened: 0x%lx -> 0x%lx\n", HandleToLong(share_handle),
          HandleToLong(memhandle));

    Sleep(50);

    if (!SpoutDXToCGetSenderInfo(spout, &info) ||
        info.shareHandle != share_handle) {
        WARN("Texture changed out under us, trying again later (0x%lx -> "
             "0x%lx)\n",
             HandleToLong(share_handle), HandleToLong(info.shareHandle));
        ret.flags |= RECEIVER_TEXTURE_INVALID;
        NtClose(memhandle);
        return ret;
    }

    ret.width = info.width;
    ret.height = info.height;
    ret.format = info.format;
    ret.usage = info.usage;

    WARN("Update DX Texture\n");
    if (!SpoutDXToCUpdateDXTexture(spout, &info)) {
        WARN("Failed to update DX texture\n");
        ret.flags |= RECEIVER_TEXTURE_INVALID;
        NtClose(memhandle);
        return ret;
    }

    uint32_t ret_size;
    struct DxvkSharedTextureMetadata metadata;

    if (get_shared_metadata(memhandle, &metadata, sizeof(metadata),
                            &ret_size) != STATUS_SUCCESS) {
        TRACE("-> metadata failed\n");
        goto no_metadata;
    }

    if (ret_size != sizeof(metadata)) {
        ERR("Metadata size mismatch, expected 0x%x, got 0x%x\n",
            (int)sizeof(metadata), ret_size);
        goto no_metadata;
    }

    TRACE("DX texture metadata:\n");
    TRACE("Width          = %d\n", metadata.Width);
    TRACE("Height         = %d\n", metadata.Height);
    TRACE("MipLevels      = %d\n", metadata.MipLevels);
    TRACE("ArraySize      = %d\n", metadata.ArraySize);
    TRACE("Format         = %d\n", metadata.Format);
    TRACE("SampleDesc     = %d, %d\n", metadata.SampleDesc.Count,
          metadata.SampleDesc.Quality);
    TRACE("Usage          = %d\n", metadata.Usage);
    TRACE("BindFlags      = %d\n", metadata.BindFlags);
    TRACE("CPUAccessFlags = %d\n", metadata.CPUAccessFlags);
    TRACE("MiscFlags      = %d\n", metadata.MiscFlags);
    TRACE("TextureLayout  = %d\n", metadata.TextureLayout);

    // Sanity check
    if (!metadata.Width || !metadata.Height) {
        ERR("Metadata is invalid\n");
        goto no_metadata;
    }

    ret.width = metadata.Width;
    ret.height = metadata.Height;
    ret.format = metadata.Format;
    ret.bind_flags = metadata.BindFlags;

    struct shared_resource_info shared_resource_info;

no_metadata:
    if (get_shared_info(memhandle, &shared_resource_info)) {
        TRACE("-> info failed\n");
        goto no_resource_size;
    }

    ret.resource_size = shared_resource_info.resource_size;

no_resource_size:
    if (NtDeviceIoControlFile(memhandle, NULL, NULL, NULL, &iosb,
                              IOCTL_SHARED_GPU_RESOURCE_GET_UNIX_RESOURCE, NULL,
                              0, &unix_resource, sizeof(unix_resource))) {
        ret.flags |= RECEIVER_TEXTURE_INVALID;
        TRACE("-> kmt handle failed\n");
        NtClose(memhandle);
        return ret;
    }

    status = wine_server_handle_to_fd(wine_server_ptr_handle(unix_resource),
                                      GENERIC_ALL, &fd, NULL);
    NtClose(wine_server_ptr_handle(unix_resource));
    NtClose(memhandle);
    if (status != STATUS_SUCCESS) {
        ret.flags |= RECEIVER_TEXTURE_INVALID;
        TRACE("-> failed to convert handle to fd\n");
        return ret;
    }

    TRACE("New texture OPAQUE fd: %d\n", fd);

    ret.opaque_fd = fd;
    ret.flags |= RECEIVER_TEXTURE_UPDATED;
    receiver->force_update = false;

    return ret;
}

static void update_receiver(struct receiver *receiver) {
    struct source_info new_info = get_receiver_info(receiver);

    if (!receiver->source) {
        if (new_info.flags == RECEIVER_TEXTURE_UPDATED) {
            struct create_source_params params = {
                .sender_name = receiver->name,
                .receiver = receiver,
                .info = new_info,
            };
            TRACE("Creating source\n");
            UNIX_CALL(create_source, &params);
            receiver->source = params.ret_source;
            if (receiver->source) {
                receiver->thread =
                    CreateThread(NULL, 0, receiver_thread, receiver, 0, 0);
            } else {
                TRACE("Source creation failed\n");
            }
        }
        return;
    }

    if (new_info.flags != receiver->info.flags ||
        (new_info.flags & RECEIVER_TEXTURE_UPDATED)) {
        struct update_source_params params = {
            .source = receiver->source,
            .info = new_info,
        };
        NTSTATUS ret = UNIX_CALL(update_source, &params);
        if (ret == STATUS_NO_SUCH_DEVICE) {
            ERR("Source '%s' had a fatal error\n", receiver->name);
            trigger_restart();
            return;
        }
        receiver->info = new_info;
    }
}

static void update_receivers(void) {
    for (uint32_t i = 0; i < num_receivers; i++)
        update_receiver(receivers[i]);
}

static struct receiver *find_receiver(const char *name) {
    for (uint32_t i = 0; i < num_receivers; i++)
        if (!strcmp(receivers[i]->name, name))
            return receivers[i];
    return NULL;
}

static void add_receiver(const char *name) {
    SPOUTDXTOC_RECEIVER *spout = SpoutDXToCNewReceiver(name);
    if (!spout) {
        TRACE("Failed to create receiver for %s\n", name);
        return;
    }

    struct receiver *receiver = calloc(1, sizeof(struct receiver));

    receiver->name = strdup(name);
    receiver->source = NULL;
    receiver->spout = spout;
    receiver->thread = NULL;

    num_receivers++;
    receivers = realloc(receivers, sizeof(struct receiver) * num_receivers);
    receivers[num_receivers - 1] = receiver;
}

static void remove_receiver(struct receiver *receiver) {
    TRACE("Destroying source %s\n", receiver->name);
    if (receiver->source)
        UNIX_CALL(destroy_source, receiver->source);

    TRACE("Joining thread for %s\n", receiver->name);
    if (receiver->thread)
        WaitForSingleObject(receiver->thread, INFINITE);

    TRACE("Freeing receiver for %s\n", receiver->name);
    SpoutDXToCFreeReceiver(receiver->spout);

    for (uint32_t i = 0; i < num_receivers; i++) {
        if (receivers[i] == receiver) {
            memmove(&receivers[i], &receivers[i + 1],
                    sizeof(struct receiver) * (num_receivers - i - 1));
            num_receivers--;
            goto free;
        }
    }
    ERR("Did not find receiver %p (%s)\n", receiver, receiver->name);

free:
    TRACE("Done removing %s\n", receiver->name);
    free(receiver->name);
    free(receiver);
}

static DWORD WINAPI sendernames_thread(void *arg) {
    TRACE("Sendernames thread started\n");

    SPOUTDXTOC_NAMELIST list = {0};
    do {
        SPOUTDXTOC_NAMELIST new_list = {0};
        SPOUTDXTOC_NAMELIST added = {0};
        SPOUTDXTOC_NAMELIST removed = {0};

        if (!SpoutDXToCGetSenderList(spout_names, &list, &new_list, &added,
                                     &removed)) {
            SpoutDXToCNamelistClear(&new_list);
            update_receivers();
            continue;
        }

        TRACE("Sender list changed\n");

        for (uint32_t i = 0; i < removed.count; i++) {
            TRACE("Removed sender: %s\n", removed.list[i]);
            struct receiver *receiver = find_receiver(removed.list[i]);
            if (receiver)
                remove_receiver(receiver);
        }

        for (uint32_t i = 0; i < added.count; i++) {
            TRACE("New sender: %s\n", added.list[i]);
            add_receiver(added.list[i]);
        }

        SpoutDXToCNamelistClear(&list);
        SpoutDXToCNamelistClear(&added);
        SpoutDXToCNamelistClear(&removed);
        list = new_list;

        update_receivers();
    } while (WaitForSingleObject(exit_event, 100) == WAIT_TIMEOUT);

    TRACE("Sendernames thread returning\n");

    while (num_receivers)
        remove_receiver(receivers[num_receivers - 1]);

    TRACE("Sendernames thread exit\n");

    return STATUS_SUCCESS;
}

static DWORD WINAPI service_handler(DWORD ctrl, DWORD event_type,
                                    LPVOID event_data, LPVOID context) {
    switch (ctrl) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        TRACE("Service control: Shutting down\n");
        service_status.dwCurrentState = SERVICE_STOP_PENDING;
        service_status.dwControlsAccepted = 0;
        SetServiceStatus(service_handle, &service_status);
        SetEvent(exit_event);
        return NO_ERROR;

    default:
        FIXME("Got service ctrl %lx\n", (long)ctrl);
        SetServiceStatus(service_handle, &service_status);
        return NO_ERROR;
    }
}

// Future use
__attribute__((unused)) static const char *_getenv(const char *var) {
    NTSTATUS ret;
    struct getenv_params params = {.var = var};

    ret = UNIX_CALL(getenv, &params);
    if (ret != STATUS_SUCCESS) {
        TRACE("unix_getenv(%s) failed (0x%lx)\n", var, ret);
        return NULL;
    }

    return params.val;
}

static void WINAPI ServiceMain(DWORD argc, LPWSTR *argv) {
    NTSTATUS ret;

    service_handle =
        RegisterServiceCtrlHandlerExW(spout2pwW, service_handler, NULL);
    if (!service_handle)
        return;

    service_status.dwServiceType = SERVICE_WIN32;
    service_status.dwCurrentState = SERVICE_START_PENDING;
    service_status.dwControlsAccepted = 0;
    service_status.dwWin32ExitCode = 0;
    service_status.dwServiceSpecificExitCode = 0;
    service_status.dwCheckPoint = 1;
    service_status.dwWaitHint = 15000;
    SetServiceStatus(service_handle, &service_status);

    TRACE("Loading unix calls\n");

    ret = __wine_init_unix_call();
    if (ret != STATUS_SUCCESS) {
        ERR("Failed to init unix libs error %lx\n", (long)ret);
        goto stop;
    }

    TRACE("Initializing spoutdxtoc.dll\n");

restart:

    // NOTE: There is no point continuing if it is.
    spout_names = SpoutDXToCNewSenderNames();
    if (spout_names == NULL) {
        ERR("spoutdxtoc.dll is a stub\n");
        goto stop;
    }

    TRACE("Starting up libfunnel\n");

    struct startup_params params = {
        .lock_texture = (UINT_PTR)lock_texture,
        .unlock_texture = (UINT_PTR)unlock_texture,
    };

    ret = UNIX_CALL(startup, &params);
    if (ret != STATUS_SUCCESS) {
        ERR("Failed to init libfunnel error %lx\n", (long)ret);
        goto stop;
    }

    TRACE("Starting service\n");

    exit_event = CreateEventW(NULL, TRUE, FALSE, NULL);

    TRACE("Starting sendernames thread\n");
    sendernames_thread_handle =
        CreateThread(NULL, 0, sendernames_thread, NULL, 0, 0);
    TRACE("Sendernames thread created\n");

    service_status.dwCurrentState = SERVICE_RUNNING;
    service_status.dwControlsAccepted =
        SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    service_status.dwCheckPoint = 0;
    service_status.dwWaitHint = 0;
    SetServiceStatus(service_handle, &service_status);

    TRACE("Waiting for exit event\n");
    WaitForMultipleObjects(1, &exit_event, FALSE, INFINITE);

    SetEvent(exit_event);

    if (sendernames_thread_handle != NULL) {
        TRACE("Stopping sender names thread\n");
        WaitForSingleObject(sendernames_thread_handle, INFINITE);
    }

    TRACE("Shutting down libfunnel\n");
    UNIX_CALL(teardown, NULL);

    TRACE("Freeing sender names\n");
    SpoutDXToCFreeSenderNames(spout_names);

    if (do_restart) {
        do_restart = false;
        goto restart;
    }

stop:
    service_status.dwCurrentState = SERVICE_STOPPED;
    service_status.dwControlsAccepted = 0;
    service_status.dwCheckPoint = 0;
    service_status.dwWaitHint = 0;
    SetServiceStatus(service_handle, &service_status);

    TRACE("Service stopped\n");
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow) {
    static const SERVICE_TABLE_ENTRYW service_table[] = {
        {spout2pwW, ServiceMain}, {NULL, NULL}};

    bool found = false;
    for (int i = 0; i < 100; i++) {
        char buf[16];
        sprintf(buf, "WINEDLLDIR%d", i);
        const char *val = getenv(buf);
        if (!val)
            break;
        TRACE("Check DLL path: %s=%s\n", buf, val);
        size_t len = strlen(val);
        const char *match = "\\spout2pw-dlls";
        size_t mlen = strlen(match);
        if (len < mlen)
            continue;
        if (strcmp(val + len - mlen, match))
            continue;
        TRACE("Spout2PW DLL path found\n");
        found = 1;
        break;
    }

    if (!found) {
        ERR("Spout2 not configured in WINEDLLPATH\n");
        return 0;
    }

    TRACE("Starting service ctrl\n");

    StartServiceCtrlDispatcherW(service_table);

    TRACE("WinMain returning\n");
    return 0;
}
