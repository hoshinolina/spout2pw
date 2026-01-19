#pragma once

#include <d3d11.h>
#include <d3d11_1.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SpoutDXToCSenderNames SPOUTDXTOC_SENDERNAMES;
typedef struct SpoutDXToCReceiver SPOUTDXTOC_RECEIVER;

typedef struct {
    char **list;
    uint32_t count;
} SPOUTDXTOC_NAMELIST;

typedef struct {
    uint8_t changed;
    HANDLE shareHandle;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t usage;
    uint32_t adapterId;
} SPOUTDXTOC_SENDERINFO;

/* Returns NULL if Wine stub in use */
SPOUTDXTOC_SENDERNAMES *SpoutDXToCNewSenderNames(void);

void SpoutDXToCFreeSenderNames(SPOUTDXTOC_SENDERNAMES *self);

int SpoutDXToCGetSenderCount(SPOUTDXTOC_SENDERNAMES *self);

bool SpoutDXToCGetSender(SPOUTDXTOC_SENDERNAMES *self, int64_t index,
                         char **sendername);

/* Returns a NULL-terminated list */
char **SpoutDXToCGetSenderListSimple(SPOUTDXTOC_SENDERNAMES *self,
                                     uint32_t *ret_count);

void SpoutDXToCNamelistClear(SPOUTDXTOC_NAMELIST *namelist);

/* TODO: Document it */
bool SpoutDXToCGetSenderList(SPOUTDXTOC_SENDERNAMES *self,
                             SPOUTDXTOC_NAMELIST *old_list,
                             SPOUTDXTOC_NAMELIST *ret_senders,
                             SPOUTDXTOC_NAMELIST *ret_added,
                             SPOUTDXTOC_NAMELIST *ret_removed);

SPOUTDXTOC_RECEIVER *SpoutDXToCNewReceiver(const char *SenderName);
void SpoutDXToCFreeReceiver(SPOUTDXTOC_RECEIVER *self);
bool SpoutDXToCIsConnected(SPOUTDXTOC_RECEIVER *self);
bool SpoutDXToCGetSenderInfo(SPOUTDXTOC_RECEIVER *self,
                             SPOUTDXTOC_SENDERINFO *info);
bool SpoutDXToCCheckTextureAccess(SPOUTDXTOC_RECEIVER *self);
bool SpoutDXToCAllowTextureAccess(SPOUTDXTOC_RECEIVER *self);
bool SpoutDXToCGetFrameCount(SPOUTDXTOC_RECEIVER *self, uint64_t *framecount);

#ifdef __cplusplus
}
#endif
