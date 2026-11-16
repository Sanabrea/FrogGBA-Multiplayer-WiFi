struct productStruct { int unknown; char product[9]; char unk[3]; };
/* Real hardware layout (152 bytes, packed), as in PPSSPP SceNetAdhocctlPeerInfoEmu */
typedef struct __attribute__((packed)) SceNetAdhocctlPeerInfo { uint32_t next; char nickname[128]; unsigned char mac[6]; uint16_t padding; uint32_t flags; uint64_t last_recv; } SceNetAdhocctlPeerInfo;
int sceNetAdhocctlInit(int stacksize, int priority, struct productStruct *product);
int sceNetAdhocctlTerm(void);
int sceNetAdhocctlConnect(const char *name);
int sceNetAdhocctlDisconnect(void);
int sceNetAdhocctlGetState(int *event);
int sceNetAdhocctlGetPeerList(int *length, void *buf);
