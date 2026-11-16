int sceNetInit(int poolsize, int calloutprio, int calloutstack, int netintrprio, int netintrstack);
int sceNetTerm(void);
int sceNetGetLocalEtherAddr(unsigned char *mac);
