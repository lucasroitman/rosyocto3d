/*********************************************************************
 *
 * $Id: ythread.c 12590 2013-09-02 13:12:44Z seb $
 *
 * OS-independent thread and synchronization library
 *
 * - - - - - - - - - License information: - - - - - - - - - 
 *
 *  Copyright (C) 2011 and beyond by Yoctopuce Sarl, Switzerland.
 *
 *  Yoctopuce Sarl (hereafter Licensor) grants to you a perpetual
 *  non-exclusive license to use, modify, copy and integrate this
 *  file into your software for the sole purpose of interfacing 
 *  with Yoctopuce products. 
 *
 *  You may reproduce and distribute copies of this file in 
 *  source or object form, as long as the sole purpose of this 
 *  code is to interface with Yoctopuce products. You must retain 
 *  this notice in the distributed source file.
 *
 *  You should refer to Yoctopuce General Terms and Conditions
 *  for additional information regarding your rights and 
 *  obligations.
 *
 *  THE SOFTWARE AND DOCUMENTATION ARE PROVIDED "AS IS" WITHOUT
 *  WARRANTY OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING 
 *  WITHOUT LIMITATION, ANY WARRANTY OF MERCHANTABILITY, FITNESS 
 *  FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO
 *  EVENT SHALL LICENSOR BE LIABLE FOR ANY INCIDENTAL, SPECIAL,
 *  INDIRECT OR CONSEQUENTIAL DAMAGES, LOST PROFITS OR LOST DATA, 
 *  COST OF PROCUREMENT OF SUBSTITUTE GOODS, TECHNOLOGY OR 
 *  SERVICES, ANY CLAIMS BY THIRD PARTIES (INCLUDING BUT NOT 
 *  LIMITED TO ANY DEFENSE THEREOF), ANY CLAIMS FOR INDEMNITY OR
 *  CONTRIBUTION, OR OTHER SIMILAR COSTS, WHETHER ASSERTED ON THE
 *  BASIS OF CONTRACT, TORT (INCLUDING NEGLIGENCE), BREACH OF
 *  WARRANTY, OR OTHERWISE.
 *
 *********************************************************************/

#include "yocto3d/yapi/ythread.h"
#define __FILE_ID__  "ythread"


#ifdef WINDOWS_API

static DWORD yTlsBucket = TLS_OUT_OF_INDEXES;
static DWORD yNextThreadIdx = 1;

void yCreateEvent(yEvent *event)
{
	*event= CreateEvent(0,0,0,0);
}

void yCreateManualEvent(yEvent *event,int initialState)
{
    *event= CreateEvent(0,TRUE,initialState!=0,0);
}


void ySetEvent(yEvent *ev)
{
	SetEvent(*ev);
}

void yResetEvent(yEvent *ev)
{
    ResetEvent(*ev);
}



int    yWaitForEvent(yEvent *ev,int time)
{
	DWORD usec;
	DWORD res;
	if(time<=0){
		usec=INFINITE;
	}else{
		usec=time;
	}
	res = WaitForSingleObject(*ev,usec);	
	return res ==WAIT_OBJECT_0;
}

void   yCloseEvent(yEvent *ev)
{
	CloseHandle(*ev);
}


static int    yCreateDetachedThreadEx(osThread *th_hdl,void* (*fun)(void *), void *arg)
{
    *th_hdl = CreateThread( 
                    NULL,                   // default security attibutes
                    0,                      // use default stack size  
                    (LPTHREAD_START_ROUTINE)fun,   // thread function name
                    arg,                    // argument to thread function 
                    0,                      // use default creation flags 
                    NULL);
    if (*th_hdl==NULL) {
        return -1;
    }
    return 0;
}


static void    yReleaseDetachedThreadEx(osThread *th_hdl)
{
    CloseHandle(*th_hdl);
}


static int    yWaitEndThread(osThread *th)
{
    DWORD result = WaitForSingleObject(*th, INFINITE);
    return result==WAIT_OBJECT_0?0:-1;
}

static void yKillThread(osThread *th)
{
	TerminateThread(*th,0);
}


int    yThreadIndex(void)
{
    DWORD res;
    
    if(yTlsBucket == TLS_OUT_OF_INDEXES) {
        // Only happens the very first time, from main thread
        yTlsBucket = TlsAlloc();
    }
    res = (DWORD)TlsGetValue(yTlsBucket);
    if(res == 0) {
        // tiny risk of race condition, but thread idx is only
        // used for debug log purposes and is not sensitive
        res = yNextThreadIdx++;
        TlsSetValue(yTlsBucket, (void *)res);
    }
    return res;
}

#else
#include <sys/time.h>
#include <pthread.h>
#include <errno.h>

static pthread_once_t yInitKeyOnce = PTHREAD_ONCE_INIT;
static pthread_key_t yTsdKey;
static unsigned yNextThreadIdx = 1;

static void initTsdKey()
{
    pthread_key_create(&yTsdKey, NULL);
}

void yCreateEvent(yEvent *ev)
{
    pthread_cond_init(&ev->cond, NULL);
    pthread_mutex_init(&ev->mtx, NULL);
    ev->verif=0;
    ev->autoreset=1;
}

void yCreateManualEvent(yEvent *ev,int initialState)
{
    pthread_cond_init(&ev->cond, NULL);
    pthread_mutex_init(&ev->mtx, NULL);
    ev->verif=initialState>0;
    ev->autoreset=0;
}

void    ySetEvent(yEvent *ev)
{
    pthread_mutex_lock(&ev->mtx);
    ev->verif=1;
    // set verif to 1 because pthread condition seems
    // to allow conditional wait to exit event if nobody
    // has set the alarm (see google or linux books of seb)
    pthread_cond_signal(&ev->cond);
    pthread_mutex_unlock(&ev->mtx);

}

void    yResetEvent(yEvent *ev)
{
    pthread_mutex_lock(&ev->mtx);
    ev->verif=0;
    pthread_mutex_unlock(&ev->mtx);

}


int   yWaitForEvent(yEvent *ev,int time)
{
    int retval;
    pthread_mutex_lock(&ev->mtx);
    if(!ev->verif){
        if(time>0){
            struct timeval now;
            struct timespec later;
            gettimeofday(&now, NULL);
            later.tv_sec=now.tv_sec+ time/1000;
            later.tv_nsec=now.tv_usec*1000 +(time%1000)*1000000;
            if(later.tv_nsec>=1000000000){
                later.tv_sec++;
                later.tv_nsec-=1000000000;
            }
            pthread_cond_timedwait(&ev->cond, &ev->mtx, &later);
            
        }else{
            pthread_cond_wait(&ev->cond,&ev->mtx);
        }
    }
    retval=ev->verif;
    if(ev->autoreset)
        ev->verif=0;
    pthread_mutex_unlock(&ev->mtx);
    return retval;
	
}
void   yCloseEvent(yEvent *ev)
{
    pthread_cond_destroy(&ev->cond);
    pthread_mutex_destroy(&ev->mtx);
}

static int    yCreateDetachedThreadEx(osThread *th,void* (*fun)(void *), void *arg)
{
    pthread_attr_t attr;
    int result;
    
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    
    if(pthread_create(th, &attr ,fun,arg)!=0){
        result=-1;
    }else{
        result=0;
    }
    pthread_attr_destroy(&attr);
    
	return result;
}

static void    yReleaseDetachedThreadEx(osThread *th_hdl)
{
}



static int yWaitEndThread(osThread *th)
{
    return pthread_join(*th,NULL);
}


static void yKillThread(osThread *th)
{
    pthread_cancel(*th);
}

int    yThreadIndex(void)
{
    int res;
    
    pthread_once(&yInitKeyOnce, initTsdKey);
    res = (int)((u8 *)pthread_getspecific(yTsdKey)-(u8 *)NULL);
    if(!res) {
        // tiny risk of race condition, but thread idx is only
        // used for debug log purposes and is not sensitive
        res = yNextThreadIdx++;
        pthread_setspecific(yTsdKey, (void*)((u8 *)NULL + res));
    }
    return res;
}

#endif




int    yCreateDetachedThread(void* (*fun)(void *), void *arg)
{
	osThread th_hdl;
	if(yCreateDetachedThreadEx(&th_hdl,fun,arg)<0){
		return -1;
	}
	yReleaseDetachedThreadEx(&th_hdl);
	return 0;
}





int    yThreadCreate(yThread *yth,void* (*fun)(void *), void *arg)
{
    if(yth->st == YTHREAD_RUNNING)
        return 0; // allready started nothing to do
    if(yth->st == YTHREAD_NOT_STARTED){
        yth->ctx = arg;
        yCreateEvent(&yth->ev);
        if(yCreateDetachedThreadEx(&yth->th,fun,yth)<0){
            yCloseEvent(&yth->ev);
            return-1;
        }
        yWaitForEvent(&yth->ev,0);
        yCloseEvent(&yth->ev);        
        return 1;
    }
    return -1;
}

int yThreadIsRunning(yThread *yth)
{
    if(yth->st ==  YTHREAD_RUNNING || yth->st == YTHREAD_MUST_STOP)
		return 1;
	return 0;
}

void   yThreadSignalStart(yThread *yth)
{
    //send ok to parent thread
    yth->st = YTHREAD_RUNNING;
    ySetEvent(&yth->ev);
}


void   yThreadSignalEnd(yThread *yth)
{
     yth->st = YTHREAD_STOPED;
}

void   yThreadRequestEnd(yThread *yth)
{
    if( yth->st == YTHREAD_RUNNING){
        yth->st = YTHREAD_MUST_STOP;
    }
}

int    yThreadMustEnd(yThread *yth)
{
    return yth->st != YTHREAD_RUNNING;
}

void yThreadKill(yThread *yth)
{
    
    if(yThreadIsRunning(yth)){
        yKillThread(&yth->th);
    }else{
        yWaitEndThread(&yth->th);
        yReleaseDetachedThreadEx(&yth->th);
    }
}


#ifdef DEBUG_CRITICAL_SECTION

//#include "yocto3d/yapi/yproto.h"
/* printf example */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DB_CS 128



#if 0
static void dump_YCS( yCRITICAL_SECTION *csptr)
{
    int i,s;
    yDbgCRITICAL_SECTION *ycs = & allycs[csno->no];
    const char* st=CS_STATE_STR[ycs->state];
    printf(
        "%lx:%02x:%s(%d:%s:%d -> %d:%s:%d) %s\n",(long)((void*)csno-NULL),csno->no,st,
        ycs->init.thread,ycs->init.fileid,ycs->init.lineno,
        ycs->release.thread,ycs->release.fileid,ycs->release.lineno,
        "");
    printf(
        "last enter %d:%s:%d  last Leave: %d:%s:%d\n",ycs->enter.thread,ycs->enter.fileid,ycs->enter.lineno,
        ycs->leave.thread,ycs->leave.fileid,ycs->leave.lineno);



    for(s=0 ; s<=CS_RELEASED; s++){
        for (i=0;i<nbycs;i++){
            ycs = & allycs[i];
            st=CS_STATE_STR[ycs->state];
            if(ycs->state!=s)
                continue;
            printf(
                "%02x:%s(%d:%s:%d -> %d:%s:%d) %s\n",i,st,
                ycs->init.thread,ycs->init.fileid,ycs->init.lineno,
                ycs->release.thread,ycs->release.fileid,ycs->release.lineno,
                i==csno->no?"<----":"");
        }
    }
}
#endif

static u32 nbycs=0;

#ifdef __arm__
#define CS_BREAK {while(1);}
#else
#if defined(WINDOWS_API) && (_MSC_VER)
#define CS_BREAK { _asm {int 3}}
#else
#define CS_BREAK  {__asm__("int3");}
#endif
#endif

#define CS_ASSERT(x)   if(!(x)){printf("ASSERT FAILED:%s:%d (%s:%d)\n",__FILE__ , __LINE__,fileid,lineno);CS_BREAK}

static void pushCSAction(const char* fileid, int lineno,yCRITICAL_SECTION_ST *csptr,YCS_ACTION action)
{
    memmove(&csptr->last_actions[1],&csptr->last_actions[0],sizeof(YCS_LOC)*(YCS_NB_TRACE-1));
    csptr->last_actions[0].thread= yThreadIndex();
    csptr->last_actions[0].fileid=fileid;
    csptr->last_actions[0].lineno=lineno;
    csptr->last_actions[0].action=action;
}


void yDbgInitializeCriticalSection(const char* fileid, int lineno, yCRITICAL_SECTION *csptr)
{
    int res;
    
    *csptr = malloc(sizeof(yCRITICAL_SECTION_ST));
    memset(*csptr,0,sizeof(yCRITICAL_SECTION_ST));
    printf("NEW CS on %s:%d:%p (%d)\n",fileid,lineno,(*csptr),nbycs);
    (*csptr)->no = nbycs++;
    (*csptr)->state = YCS_ALLOCATED;    
    pushCSAction(fileid,lineno,(*csptr),YCS_INIT);
#if MICROCHIP_API
    (*csptr)->cs=0;
    res =0;
#elif defined(WINDOWS_API)
    res =0;
    InitializeCriticalSection(&((*csptr)->cs));
#else
    res = pthread_mutex_init(&((*csptr)->cs),NULL);
#endif
    EnterCriticalSection(&((*csptr)->cs));
    LeaveCriticalSection(&((*csptr)->cs));
#if 0
    CS_ASSERT(res==0);
    res = pthread_mutex_lock(&((*csptr)->cs));
    CS_ASSERT(res==0);
    res = pthread_mutex_unlock(&((*csptr)->cs));
#endif
    CS_ASSERT(res==0);
}


void yDbgEnterCriticalSection(const char* fileid, int lineno, yCRITICAL_SECTION *csptr)
{
    int res;

    CS_ASSERT((*csptr)->no < nbycs);
    CS_ASSERT((*csptr)->state == YCS_ALLOCATED);

    if((*csptr)->no ==15) {
        printf("enter CS on %s:%d:%p (%p)\n",fileid,lineno,(*csptr),&((*csptr)->cs));
    }

#if MICROCHIP_API
    (*csptr)->cs=1;
#elif defined(WINDOWS_API)
    res =0;
    EnterCriticalSection(&((*csptr)->cs));
#else
    res = pthread_mutex_lock(&((*csptr)->cs));
#endif
    CS_ASSERT(res==0);
    CS_ASSERT((*csptr)->lock == 0);
    (*csptr)->lock++;
    pushCSAction(fileid,lineno,(*csptr),YCS_LOCK);
}


int yDbgTryEnterCriticalSection(const char* fileid, int lineno, yCRITICAL_SECTION *csptr)
{
    int res;

    CS_ASSERT((*csptr)->no < nbycs);
    CS_ASSERT((*csptr)->state == YCS_ALLOCATED);

#if MICROCHIP_API
    if((*csptr)->cs)
        return 0;
    (*csptr)->cs=1;
#elif defined(WINDOWS_API)
    res = TryEnterCriticalSection(&((*csptr)->cs));
    if(res==0)
        return 0;
    CS_ASSERT(res==1);
#else
    res = pthread_mutex_trylock(&((*csptr)->cs));
    if(res ==EBUSY)
        return 0;
    CS_ASSERT(res==0);
#endif
    CS_ASSERT((*csptr)->lock == 0);
    (*csptr)->lock++;
    pushCSAction(fileid,lineno,(*csptr),YCS_LOCKTRY);
    return 1;
}


void yDbgLeaveCriticalSection(const char* fileid, int lineno, yCRITICAL_SECTION *csptr)
{
    int res;

    CS_ASSERT((*csptr)->no < nbycs);
    CS_ASSERT((*csptr)->state == YCS_ALLOCATED);
    CS_ASSERT((*csptr)->lock == 1);
    if((*csptr)->no ==15) {
        printf("leave CS on %s:%d:%p (%p)\n",fileid,lineno,(*csptr),&((*csptr)->cs));
    }

    (*csptr)->lock--;
    pushCSAction(fileid,lineno,(*csptr),YCS_RELEASE);

#if MICROCHIP_API
    (*csptr)->cs=0;
    res =0;
#elif defined(WINDOWS_API)
    res =0;    
    LeaveCriticalSection(&((*csptr)->cs));
#else
    res = pthread_mutex_unlock(&((*csptr)->cs));
#endif
    CS_ASSERT(res==0);
}

void yDbgDeleteCriticalSection(const char* fileid, int lineno, yCRITICAL_SECTION *csptr)
{
    int res;

    CS_ASSERT((*csptr)->no < nbycs);
    CS_ASSERT((*csptr)->state == YCS_ALLOCATED);
    CS_ASSERT((*csptr)->lock == 0);

    if((*csptr)->no ==15) {
        printf("delete CS on %s:%d:%p (%p)\n",fileid,lineno,(*csptr),&((*csptr)->cs));
    }

#if MICROCHIP_API
    (*csptr)->cs=0xCA;
    res = 0;
#elif defined(WINDOWS_API)
    res = 0;
    DeleteCriticalSection(&((*csptr)->cs));
#else
    res = pthread_mutex_destroy(&((*csptr)->cs));
#endif
    CS_ASSERT(res==0);
    (*csptr)->state = YCS_DELETED;
    pushCSAction(fileid,lineno,(*csptr),YCS_DELETE);
}

#elif !defined(MICROCHIP_API)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


typedef struct {
#if defined(WINDOWS_API)
    CRITICAL_SECTION             cs;
#else
    pthread_mutex_t              cs;
#endif
} yCRITICAL_SECTION_ST;


void yInitializeCriticalSection(yCRITICAL_SECTION *cs)
{
    yCRITICAL_SECTION_ST *ycsptr;
    ycsptr = (yCRITICAL_SECTION_ST*) malloc(sizeof(yCRITICAL_SECTION_ST));
    memset(ycsptr,0,sizeof(yCRITICAL_SECTION_ST));
#if defined(WINDOWS_API)
    InitializeCriticalSection(&(ycsptr->cs));
#else
    pthread_mutex_init(&(ycsptr->cs),NULL);
#endif
    *cs = ycsptr;
}

void yEnterCriticalSection(yCRITICAL_SECTION *cs)
{
    yCRITICAL_SECTION_ST *ycsptr = (yCRITICAL_SECTION_ST*) (*cs);
#if defined(WINDOWS_API)
    EnterCriticalSection(&(ycsptr->cs));
#else
    pthread_mutex_lock(&(ycsptr->cs));
#endif
}

int yTryEnterCriticalSection(yCRITICAL_SECTION *cs)
{
    yCRITICAL_SECTION_ST *ycsptr = (yCRITICAL_SECTION_ST*) (*cs);
#if defined(WINDOWS_API)
    return TryEnterCriticalSection(&(ycsptr->cs));
#else
    {
        int res = pthread_mutex_trylock(&(ycsptr->cs));
        if(res ==EBUSY)
            return 0;
        return 1;
    }
#endif
}

void yLeaveCriticalSection(yCRITICAL_SECTION *cs)
{
    yCRITICAL_SECTION_ST *ycsptr = (yCRITICAL_SECTION_ST*) (*cs);
#if defined(WINDOWS_API)
    LeaveCriticalSection(&(ycsptr->cs));
#else
    pthread_mutex_unlock(&(ycsptr->cs));
#endif
}

void yDeleteCriticalSection(yCRITICAL_SECTION *cs)
{
    yCRITICAL_SECTION_ST *ycsptr = (yCRITICAL_SECTION_ST*) (*cs);
#if defined(WINDOWS_API)
    DeleteCriticalSection(&(ycsptr->cs));
#else
    pthread_mutex_destroy(&(ycsptr->cs));
#endif
    free(*cs);
    *cs=NULL;
}

#endif


