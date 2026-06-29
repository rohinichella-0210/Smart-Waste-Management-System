#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "validation.h"

void sanitize(char *s){
    for(char *p=s;*p;p++) if(!isprint((unsigned char)*p)) *p='_';
}
char *jstr(const char *json,const char *key,char *out,int maxlen){
    char needle[128]; snprintf(needle,sizeof(needle),"\"%s\"",key);
    const char *p=strstr(json,needle); if(!p)return NULL;
    p+=strlen(needle);
    while(*p==' '||*p==':') p++;
    if(*p=='"'){ p++; int i=0;
        while(*p&&*p!='"'&&i<maxlen-1) out[i++]=*p++;
        out[i]='\0'; sanitize(out); return out;
    }
    return NULL;
}
int jint(const char *json,const char *key,int *out){
    char needle[128]; snprintf(needle,sizeof(needle),"\"%s\"",key);
    const char *p=strstr(json,needle); if(!p)return 0;
    p+=strlen(needle);
    while(*p==' '||*p==':') p++;
    if(isdigit((unsigned char)*p)||*p=='-'){*out=atoi(p);return 1;}
    return 0;
}
int jflt(const char *json,const char *key,float *out){
    char needle[128]; snprintf(needle,sizeof(needle),"\"%s\"",key);
    const char *p=strstr(json,needle); if(!p)return 0;
    p+=strlen(needle);
    while(*p==' '||*p==':') p++;
    if(isdigit((unsigned char)*p)||*p=='-'||*p=='.'){*out=(float)atof(p);return 1;}
    return 0;
}
float fclamp(float v,float lo,float hi){return v<lo?lo:(v>hi?hi:v);}
int   iclamp(int v,int lo,int hi){return v<lo?lo:(v>hi?hi:v);}
