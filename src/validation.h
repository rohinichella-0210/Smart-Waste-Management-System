/* validation.h */
#ifndef VALIDATION_H
#define VALIDATION_H
char  *jstr(const char *json, const char *key, char *out, int maxlen);
int    jint(const char *json, const char *key, int *out);
int    jflt(const char *json, const char *key, float *out);
void   sanitize(char *s);
float  fclamp(float v, float lo, float hi);
int    iclamp(int v, int lo, int hi);
#endif
