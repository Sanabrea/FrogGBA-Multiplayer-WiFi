#define PSP_NET_MODULE_COMMON 1
#define PSP_NET_MODULE_ADHOC 2
int sceUtilityLoadNetModule(int module);
int sceUtilityUnloadNetModule(int module);
