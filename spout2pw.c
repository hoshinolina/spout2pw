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
static HANDLE exit_event, wine_exit_event;
static SERVICE_STATUS_HANDLE service_handle;

static HANDLE sendernames_thread_handle = 0;
static SPOUTDXTOC_SENDERNAMES *spout_names = NULL;

static DWORD WINAPI sendernames_thread(void *arg);

static bool do_restart = false;

#define IOCTL_SHARED_GPU_RESOURCE_GET_UNIX_RESOURCE                            \
    CTL_CODE(FILE_DEVICE_VIDEO, 3, METHOD_BUFFERED, FILE_READ_ACCESS)

#define IOCTL_SHARED_GPU_RESOURCE_OPEN                                         \
    CTL_CODE(FILE_DEVICE_VIDEO, 1, METHOD_BUFFERED, FILE_WRITE_ACCESS)

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
    struct source_info ret = {.dmabuf_fd = -1};

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

    TRACE("Sender %dx%d fmt=%d handle=0x%lx changed=%d\n", info.width,
          info.height, info.format, (long)(intptr_t)info.shareHandle,
          info.changed);

    if (info.changed || receiver->force_update) {
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

        TRACE("Share handle opened: 0x%lx -> 0x%lx\n",
              HandleToLong(share_handle), HandleToLong(memhandle));

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

        WARN("Update DX Texture\n");
        if (!SpoutDXToCUpdateDXTexture(spout, &info)) {
            WARN("Failed to update DX texture\n");
            ret.flags |= RECEIVER_TEXTURE_INVALID;
            NtClose(memhandle);
            return ret;
        }

        if (NtDeviceIoControlFile(memhandle, NULL, NULL, NULL, &iosb,
                                  IOCTL_SHARED_GPU_RESOURCE_GET_UNIX_RESOURCE,
                                  NULL, 0, &unix_resource,
                                  sizeof(unix_resource))) {
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

        TRACE("New texture DMA-BUF fd: %d\n", fd);

        ret.dmabuf_fd = fd;
        ret.flags |= RECEIVER_TEXTURE_UPDATED;
        receiver->force_update = false;
    }

    ret.width = info.width;
    ret.height = info.height;
    ret.format = info.format;
    ret.usage = info.usage;

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
    SERVICE_STATUS status;

    status.dwServiceType = SERVICE_WIN32;
    status.dwControlsAccepted = SERVICE_ACCEPT_STOP;
    status.dwWin32ExitCode = 0;
    status.dwServiceSpecificExitCode = 0;
    status.dwCheckPoint = 0;
    status.dwWaitHint = 0;

    switch (ctrl) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        TRACE("Service control: Shutting down\n");
        status.dwCurrentState = SERVICE_STOP_PENDING;
        status.dwControlsAccepted = 0;
        SetServiceStatus(service_handle, &status);
        SetEvent(exit_event);
        return NO_ERROR;

    default:
        FIXME("Got service ctrl %lx\n", (long)ctrl);
        status.dwCurrentState = SERVICE_RUNNING;
        SetServiceStatus(service_handle, &status);
        return NO_ERROR;
    }
}

static void WINAPI ServiceMain(DWORD argc, LPWSTR *argv) {
    SERVICE_STATUS status;
    NTSTATUS ret;

    service_handle =
        RegisterServiceCtrlHandlerExW(spout2pwW, service_handler, NULL);
    if (!service_handle)
        return;

    TRACE("Loading unix call\n");

    ret = __wine_init_unix_call();
    if (ret != STATUS_SUCCESS) {
        ERR("Failed to init unix calls error %lx\n", (long)ret);
        goto stop;
    }

    ret = UNIX_CALL(libs_init, NULL);
    if (ret != STATUS_SUCCESS) {
        ERR("Failed to init unix libs error %lx\n", (long)ret);
        goto stop;
    }

    TRACE("Checking if spoutdxtoc.dll is not a stub\n");

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

    status.dwServiceType = SERVICE_WIN32;
    status.dwCurrentState = SERVICE_RUNNING;
    status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    status.dwWin32ExitCode = 0;
    status.dwServiceSpecificExitCode = 0;
    status.dwCheckPoint = 0;
    status.dwWaitHint = 10000;
    SetServiceStatus(service_handle, &status);

    TRACE("Starting sendernames thread\n");
    sendernames_thread_handle =
        CreateThread(NULL, 0, sendernames_thread, NULL, 0, 0);
    TRACE("Sendernames thread created\n");

    TRACE("Waiting for exit event\n");
    HANDLE handles[2] = {exit_event, wine_exit_event};
    WaitForMultipleObjects(2, handles, FALSE, INFINITE);

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
    status.dwCurrentState = SERVICE_STOPPED;
    status.dwControlsAccepted = 0;
    SetServiceStatus(service_handle, &status);

    TRACE("Service stopped\n");
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow) {
    static const SERVICE_TABLE_ENTRYW service_table[] = {
        {spout2pwW, ServiceMain}, {NULL, NULL}};

    TRACE("Make system\n");
    wine_exit_event = NULL;
    NtSetInformationProcess(GetCurrentProcess(), ProcessWineMakeProcessSystem,
                            &wine_exit_event, sizeof(HANDLE *));
    TRACE("Starting service ctrl\n");

    StartServiceCtrlDispatcherW(service_table);

    return 0;
}
