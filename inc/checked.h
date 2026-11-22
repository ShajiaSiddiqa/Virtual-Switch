#ifndef VSW_CHECKED_H
#define VSW_CHECKED_H
/* Fail-fast checks for operations whose failure makes continued execution unsafe.
 * Normal recoverable errors still use the calling module's explicit branches. */
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

static inline void vsw_pthread(int result,const char *operation)
{
    if(result!=0) {errno=result;perror(operation);abort();}
}
static inline void vsw_posix(long result,const char *operation)
{
    if(result<0) {perror(operation);abort();}
}
static inline void vsw_zero(int result,const char *operation)
{
    if(result!=0) {perror(operation);abort();}
}
static inline void vsw_format(int result,size_t capacity)
{
    if(result<0 || (size_t)result>=capacity) {errno=EOVERFLOW;perror("snprintf");abort();}
}
static inline void vsw_kill(pid_t process,int number)
{
    if(kill(process,number)<0 && errno!=ESRCH) {perror("kill");abort();}
}
#endif
