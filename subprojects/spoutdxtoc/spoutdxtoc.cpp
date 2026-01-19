#include "spoutdxtoc.h"

#include <algorithm>
#include <string>
#include <vector>

/* Include SpoutDX and ignore its headers warnings */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"

#include "SpoutDirectX.h"
#include "SpoutFrameCount.h"
#include "SpoutSenderNames.h"
#include "SpoutUtils.h"

#pragma GCC diagnostic pop

struct SpoutDXToCSenderNames {
    spoutSenderNames sendernames;
};

struct SpoutDXToCReceiver {
    std::string sendername;
    uint32_t lastShareHandle;
    uint32_t lastAdapterId;
    ID3D11Texture2D *sharedTexture;

    spoutSenderNames sendernames;
    spoutFrameCount frame;
    spoutDirectX dx;
};

SPOUTDXTOC_SENDERNAMES *__stdcall SpoutDXToCNewSenderNames(void) {
    spoututils::EnableSpoutLogFile("C:\\spoutlog.txt");
    SPOUTDXTOC_SENDERNAMES *p = new SpoutDXToCSenderNames();
    return p;
}

void __stdcall SpoutDXToCFreeSenderNames(SPOUTDXTOC_SENDERNAMES *self) {
    assert(self != NULL);

    delete self;
}

int __stdcall SpoutDXToCGetSenderCount(SPOUTDXTOC_SENDERNAMES *self) {
    assert(self != NULL);

    return self->sendernames.GetSenderCount();
}

#define NAME_MAX_SIZE 256

bool __stdcall SpoutDXToCGetSender(SPOUTDXTOC_SENDERNAMES *self, int64_t index,
                                   char **sendername) {
    assert(self != NULL);
    assert(sendername != NULL && *sendername == NULL);

    *sendername = (char *)calloc(1, NAME_MAX_SIZE * sizeof(char));

    if (self->sendernames.GetSender((int)index, *sendername, NAME_MAX_SIZE))
        return true;

    free(*sendername);
    *sendername = NULL;

    return false;
}

static void vec_to_null_term_clist(std::vector<std::string> &vector,
                                   char ***list) {
    char **name;

    *list = (char **)calloc(vector.size() + 1, sizeof(char *));

    name = *list;
    for (std::string &s : vector) {
        *name = strdup(s.c_str());
        name++;
    }

    name = *list;
    assert(name[vector.size()] == NULL);
}

char **__stdcall SpoutDXToCGetSenderListSimple(SPOUTDXTOC_SENDERNAMES *self,
                                               uint32_t *ret_count) {
    std::vector<std::string> senderlist;
    char **list = NULL;

    assert(self != NULL);

    int nSenders = self->sendernames.GetSenderCount();
    if (nSenders > 0) {
        char sendername[256]{};
        for (int i = 0; i < nSenders; i++) {
            if (self->sendernames.GetSender(i, sendername))
                senderlist.push_back(sendername);
        }
    }

    vec_to_null_term_clist(senderlist, &list);

    if (ret_count != NULL)
        *ret_count = senderlist.size();

    return list;
}

void __stdcall SpoutDXToCNamelistClear(SPOUTDXTOC_NAMELIST *namelist) {
    assert(namelist != NULL);

    if (namelist->list == NULL)
        return;

    for (uint32_t i = 0; namelist->list[i] != NULL; i++)
        free(namelist->list[i]);

    free(namelist->list);
    namelist->list = NULL;
}

bool __stdcall SpoutDXToCGetSenderList(SPOUTDXTOC_SENDERNAMES *self,
                                       SPOUTDXTOC_NAMELIST *old_list,
                                       SPOUTDXTOC_NAMELIST *ret_senders,
                                       SPOUTDXTOC_NAMELIST *ret_added,
                                       SPOUTDXTOC_NAMELIST *ret_removed) {
    std::vector<std::string> senderlist, list, removed;

    assert(self != NULL);

    if (ret_senders == NULL) {
        assert(ret_added != NULL && ret_added->list == NULL);
        assert(ret_removed != NULL && ret_removed->list == NULL);
    } else {
        assert(ret_senders->list == NULL);
        assert(ret_added == NULL || ret_added->list == NULL);
        assert(ret_removed == NULL || ret_removed->list == NULL);
    }

    int nSenders = self->sendernames.GetSenderCount();
    if (nSenders > 0) {
        char sendername[256]{};
        for (int i = 0; i < nSenders; i++) {
            if (self->sendernames.GetSender(i, sendername))
                list.push_back(sendername);
        }
    }
    senderlist = list;

    for (size_t i = 0; i < old_list->count; i++) {
        std::string sender(old_list->list[i]);
        auto it = std::find(list.begin(), list.end(), sender);

        if (it != list.end())
            list.erase(it);
        else
            removed.push_back(sender);
    }

    if (list.empty() && removed.empty())
        return false;

    if (ret_senders != NULL) {
        vec_to_null_term_clist(senderlist, &ret_senders->list);
        ret_senders->count = senderlist.size();
    }

    if (ret_added != NULL) {
        vec_to_null_term_clist(list, &ret_added->list);
        ret_added->count = list.size();
    }

    if (ret_removed != NULL) {
        vec_to_null_term_clist(removed, &ret_removed->list);
        ret_removed->count = removed.size();
    }

    return true;
}

SPOUTDXTOC_RECEIVER *__stdcall SpoutDXToCNewReceiver(const char *SenderName) {
    SPOUTDXTOC_RECEIVER *p = new SpoutDXToCReceiver();

    p->sendername = std::string(SenderName);
    p->frame.CreateAccessMutex(SenderName);
    p->frame.EnableFrameCount(SenderName);
    return p;
}

void __stdcall SpoutDXToCFreeReceiver(SPOUTDXTOC_RECEIVER *self) {
    assert(self != NULL);

    self->frame.CloseAccessMutex();
    self->frame.CleanupFrameCount();

    if (self->sharedTexture) {
        self->sharedTexture->Release();
        self->sharedTexture = nullptr;
        self->dx.CloseDirectX11();
    }

    delete self;
}

bool __stdcall SpoutDXToCIsConnected(SPOUTDXTOC_RECEIVER *self) {
    assert(self != NULL);

    SharedTextureInfo info;
    if (!self->sendernames.getSharedInfo(self->sendername.c_str(), &info))
        return false;

    if (info.width == 0 || info.height == 0 || info.shareHandle == 0)
        return false;

    return true;
}

static bool InitDXTexture(SPOUTDXTOC_RECEIVER *self, uint32_t shareHandle) {
    IDXGIAdapter *pAdapter = nullptr;

    if (self->sharedTexture) {
        self->sharedTexture->Release();
        self->sharedTexture = nullptr;
        self->dx.CloseDirectX11();
    }

    const int nAdapters = self->dx.GetNumAdapters();
    for (int i = 0; i < nAdapters; i++) {
        if (!self->dx.SetAdapter(i))
            continue;

        // Set the adapter pointer for CreateDX11device to use temporarily
        self->dx.SetAdapterPointer(pAdapter);
        if (!self->dx.OpenDirectX11(nullptr))
            continue;

        // Try to open the share handle with the device created from the adapter
        if (self->dx.OpenDX11shareHandle(self->dx.GetDX11Device(),
                                         &self->sharedTexture,
                                         LongToHandle((long)shareHandle))) {
            self->lastAdapterId = i;
            return true;
        }

        self->dx.CloseDirectX11();
    }

    return false;
}

bool __stdcall SpoutDXToCGetSenderInfo(SPOUTDXTOC_RECEIVER *self,
                                       SPOUTDXTOC_SENDERINFO *info) {
    assert(self != NULL);
    assert(info != NULL);

    SharedTextureInfo sinfo;
    if (!self->sendernames.getSharedInfo(self->sendername.c_str(), &sinfo))
        return false;

    info->shareHandle = (HANDLE)(LongToHandle((long)sinfo.shareHandle));
    info->width = sinfo.width;
    info->height = sinfo.height;
    info->format = sinfo.format;
    info->usage = sinfo.usage;
    info->changed = false;

    if (self->lastShareHandle != sinfo.shareHandle) {
        if (!InitDXTexture(self, sinfo.shareHandle))
            return false;

        self->lastShareHandle = sinfo.shareHandle;
        info->changed = true;
    }
    info->adapterId = self->lastAdapterId;

    return true;
}

bool __stdcall SpoutDXToCCheckTextureAccess(SPOUTDXTOC_RECEIVER *self) {
    assert(self != NULL);
    assert(self->sharedTexture != NULL);

    return self->frame.CheckTextureAccess(self->sharedTexture);
}

bool __stdcall SpoutDXToCAllowTextureAccess(SPOUTDXTOC_RECEIVER *self) {
    assert(self != NULL);
    assert(self->sharedTexture != NULL);

    return self->frame.AllowTextureAccess(self->sharedTexture);
}

bool __stdcall SpoutDXToCGetFrameCount(SPOUTDXTOC_RECEIVER *self,
                                       uint64_t *framecount) {
    assert(self != NULL);

    bool ret = self->frame.GetNewFrame();

    if (framecount)
        *framecount = self->frame.GetSenderFrame();

    return ret;
}
