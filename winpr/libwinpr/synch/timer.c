/**
 * WinPR: Windows Portable Runtime
 * Synchronization Functions
 *
 * Copyright 2012 Marc-Andre Moreau <marcandre.moreau@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <winpr/crt.h>
#include <winpr/file.h>
#include <winpr/sysinfo.h>

#include <winpr/synch.h>

#ifndef _WIN32
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>
#include <signal.h>
#endif

#include "synch.h"

#ifndef _WIN32

#include "../handle/handle.h"

#include "../log.h"
#define TAG WINPR_TAG("synch.timer")

static BOOL TimerCloseHandle(HANDLE handle);

static BOOL TimerIsHandled(HANDLE handle)
{
	WINPR_TIMER* pTimer = (WINPR_TIMER*)handle;

	if (!pTimer || (pTimer->Type != HANDLE_TYPE_TIMER))
	{
		SetLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}

	return TRUE;
}

static int TimerGetFd(HANDLE handle)
{
	WINPR_TIMER* timer = (WINPR_TIMER*)handle;

	if (!TimerIsHandled(handle))
		return -1;

	return timer->fd;
}

static DWORD TimerCleanupHandle(HANDLE handle)
{
	int length;
	UINT64 expirations;
	WINPR_TIMER* timer = (WINPR_TIMER*)handle;

	if (!TimerIsHandled(handle))
		return WAIT_FAILED;

	if (timer->bManualReset)
		return WAIT_OBJECT_0;

  WLog_VRB(TAG, "%s: Before Read, fd=%d, WAIT_TIMEOUT=%d, WAIT_FAILED=%d, WAIT_OBJECT_0=%d", __FUNCTION__, timer->fd, WAIT_TIMEOUT, WAIT_FAILED, WAIT_OBJECT_0);
  #ifdef WITH_KQUEUE
  return WAIT_OBJECT_0;
  #endif

	length = read(timer->fd, (void*)&expirations, sizeof(UINT64));

	if (length != 8)
	{
		if (length == -1)
		{
			switch (errno)
			{
				case ETIMEDOUT:
				case EAGAIN:
					return WAIT_TIMEOUT;

				default:
					break;
			}

			WLog_ERR(TAG, "%s: timer read() failure [%d] %s", __FUNCTION__, errno, strerror(errno));
		}
		else
		{
			WLog_ERR(TAG, "%s: timer read() failure - incorrect number of bytes read", __FUNCTION__);
		}

		return WAIT_FAILED;
	}

	return WAIT_OBJECT_0;
}

BOOL TimerCloseHandle(HANDLE handle)
{
	WINPR_TIMER* timer;
	timer = (WINPR_TIMER*)handle;

	if (!TimerIsHandled(handle))
		return FALSE;

	if (!timer->lpArgToCompletionRoutine)
	{
#ifdef HAVE_SYS_TIMERFD_H

		if (timer->fd != -1)
			close(timer->fd);

#endif
	}
	else
	{
#ifdef WITH_POSIX_TIMER
		timer_delete(timer->tid);
#elif defined(WITH_KQUEUE)
    WLog_VRB(TAG, "%s: Deleting timeout for timer %s", __FUNCTION__, timer->name);
    EV_SET(&timer->event, 1, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
    kevent(timer->fd, &timer->event, 1, NULL, 0, NULL);
#endif
	}

#if defined(__APPLE__)
	dispatch_release(timer->queue);
	dispatch_release(timer->source);

	if (timer->pipe[0] != -1)
		close(timer->pipe[0]);

	if (timer->pipe[1] != -1)
		close(timer->pipe[1]);

#endif
	free(timer->name);
	free(timer);
	return TRUE;
}

#if defined(WITH_POSIX_TIMER) || defined(WITH_KQUEUE)

static BOOL g_WaitableTimerSignalHandlerInstalled = FALSE;

static void WaitableTimerHandler(void* arg)
{
	WINPR_TIMER* timer = (WINPR_TIMER*)arg;

	if (!timer)
		return;
  WLog_VRB(TAG, "%s: Handler Start for %s", __FUNCTION__, timer->name);

	if (timer->pfnCompletionRoutine)
	{
    WLog_VRB(TAG, "%s: Calling CompletionRoutine for %s", __FUNCTION__, timer->name);
		timer->pfnCompletionRoutine(timer->lpArgToCompletionRoutine, 0, 0);

		if (timer->lPeriod)
		{
			timer->timeout.it_interval.tv_sec = (timer->lPeriod / 1000); /* seconds */
			timer->timeout.it_interval.tv_nsec =
			    ((timer->lPeriod % 1000) * 1000000); /* nanoseconds */

#ifdef WITH_POSIX_TIMER
			if ((timer_settime(timer->tid, 0, &(timer->timeout), NULL)) != 0)
			{
				WLog_ERR(TAG, "timer_settime");
			}
#endif
#if defined(WITH_KQUEUE)
      uint64_t timeout_msec = (timer->timeout.it_value.tv_sec * 1000LL) + (timer->timeout.it_value.tv_nsec /1000000LL );
      WLog_VRB(TAG, "%s: Timer triggered, rescheduling timer %s to %lu ms", __FUNCTION__, timer->name, timeout_msec);
      EV_SET(&timer->event, 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0, timeout_msec, timer);
      kevent(timer->fd, &timer->event, 1, NULL, 0, NULL);
#endif
		}
	}
}
static void WaitableTimerSignalHandler(int signum, siginfo_t* siginfo, void* arg)
{
	WINPR_TIMER* timer = siginfo->si_value.sival_ptr;
	WINPR_UNUSED(arg);

	if (!timer || (signum != SIGALRM))
		return;

	WaitableTimerHandler(timer);
}

static int InstallWaitableTimerSignalHandler(void)
{
	if (!g_WaitableTimerSignalHandlerInstalled)
	{
		struct sigaction action;
		sigemptyset(&action.sa_mask);
		sigaddset(&action.sa_mask, SIGALRM);
		action.sa_flags = SA_RESTART | SA_SIGINFO;
		action.sa_sigaction = WaitableTimerSignalHandler;
		sigaction(SIGALRM, &action, NULL);
		g_WaitableTimerSignalHandlerInstalled = TRUE;
	}

	return 0;
}

#endif

#if defined(__APPLE__)
static void WaitableTimerHandler(void* arg)
{
	UINT64 data = 1;
	WINPR_TIMER* timer = (WINPR_TIMER*)arg;

	if (!timer)
		return;

	if (timer->pfnCompletionRoutine)
		timer->pfnCompletionRoutine(timer->lpArgToCompletionRoutine, 0, 0);

	if (write(timer->pipe[1], &data, sizeof(data)) != sizeof(data))
		WLog_ERR(TAG, "failed to write to pipe");

	if (timer->lPeriod == 0)
	{
		if (timer->running)
			dispatch_suspend(timer->source);

		timer->running = FALSE;
	}
}
#endif

static int InitializeWaitableTimer(WINPR_TIMER* timer)
{
	int result = 0;

	if (!timer->lpArgToCompletionRoutine)
	{
#ifdef HAVE_SYS_TIMERFD_H
		timer->fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);

		if (timer->fd <= 0)
			return -1;

#elif defined(__APPLE__)
#elif defined(WITH_KQUEUE)
    WLog_VRB(TAG, "%s: kqueue creation for timer %s", __FUNCTION__, timer->name);
    timer->fd = kqueue();
    if(timer->fd == -1)
      {
        WLog_ERR(TAG, "kqueue() returned error: %d", timer->fd);
        free(timer);
        return -1;
      }
#else
		WLog_ERR(TAG, "%s: os specific implementation is missing", __FUNCTION__);
		result = -1;
#endif
	}
	else
	{
#ifdef WITH_POSIX_TIMER
		struct sigevent sigev;
		InstallWaitableTimerSignalHandler();
		ZeroMemory(&sigev, sizeof(struct sigevent));
		sigev.sigev_notify = SIGEV_SIGNAL;
		sigev.sigev_signo = SIGALRM;
		sigev.sigev_value.sival_ptr = (void*)timer;
		if ((timer_create(CLOCK_MONOTONIC, &sigev, &(timer->tid))) != 0)
		{
			WLog_ERR(TAG, "timer_create");
			return -1;
		}
#elif defined(WITH_KQUEUE)
    /*QUESTION: Spawn thread here to monitor kqueue and issue callbacks for APC?*/
    /*          Right now it is only polled when WaitForSingleObject is called  */
    /*          xfreerdp runs fine without this, but the APC tests fail         */
#elif defined(__APPLE__)
#else
		WLog_ERR(TAG, "%s: os specific implementation is missing", __FUNCTION__);
		result = -1;
#endif
	}

	timer->bInit = TRUE;
	return result;
}

static HANDLE_OPS ops = { TimerIsHandled, TimerCloseHandle,
	                      TimerGetFd,     TimerCleanupHandle,
	                      NULL,           NULL,
	                      NULL,           NULL,
	                      NULL,           NULL,
	                      NULL,           NULL,
	                      NULL,           NULL,
	                      NULL,           NULL,
	                      NULL,           NULL,
	                      NULL,           NULL };

/**
 * Waitable Timer
 */
static void* kqueue_monitoring_thread_start(void* arg)
{
	WINPR_TIMER *timer = (WINPR_TIMER*)arg;
  if(timer == NULL)
  {
		WLog_ERR(TAG, "%s invalid timer", __FUNCTION__);
    return NULL;
  }
  WLog_VRB(TAG, "%s: monitor thread started timer %s", __FUNCTION__, timer->name);
  struct kevent ev;
  while(true)
  {
    kevent(timer->fd, NULL, 0, &ev, 1, NULL);
    WLog_VRB(TAG, "%s: detected event for %s", __FUNCTION__, timer->name);
    WaitableTimerHandler(timer);
    WLog_VRB(TAG, "%s: Handler called for %s", __FUNCTION__, timer->name);
  }
}

HANDLE CreateWaitableTimerA(LPSECURITY_ATTRIBUTES lpTimerAttributes, BOOL bManualReset,
                            LPCSTR lpTimerName)
{
	HANDLE handle = NULL;
	WINPR_TIMER* timer;

	if (lpTimerAttributes)
		WLog_WARN(TAG, "%s [%s] does not support lpTimerAttributes", __FUNCTION__, lpTimerName);

	timer = (WINPR_TIMER*)calloc(1, sizeof(WINPR_TIMER));

	if (timer)
	{
		WINPR_HANDLE_SET_TYPE_AND_MODE(timer, HANDLE_TYPE_TIMER, WINPR_FD_READ);
		handle = (HANDLE)timer;
		timer->fd = -1;
		timer->lPeriod = 0;
		timer->bManualReset = bManualReset;
		timer->pfnCompletionRoutine = NULL;
		timer->lpArgToCompletionRoutine = NULL;
		timer->bInit = FALSE;


		if (lpTimerName)
			timer->name = strdup(lpTimerName);

		timer->ops = &ops;
#if defined(__APPLE__)

		if (pipe(timer->pipe) != 0)
			goto fail;

		timer->queue = dispatch_queue_create(TAG, DISPATCH_QUEUE_SERIAL);

		if (!timer->queue)
			goto fail;

		timer->source = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, timer->queue);

		if (!timer->source)
			goto fail;

		dispatch_set_context(timer->source, timer);
		dispatch_source_set_event_handler_f(timer->source, WaitableTimerHandler);
		timer->fd = timer->pipe[0];

		if (fcntl(timer->fd, F_SETFL, O_NONBLOCK) < 0)
			goto fail;

#endif
	}

	return handle;
#if defined(__APPLE__)
fail:
	TimerCloseHandle(handle);
	return NULL;
#endif
}

HANDLE CreateWaitableTimerW(LPSECURITY_ATTRIBUTES lpTimerAttributes, BOOL bManualReset,
                            LPCWSTR lpTimerName)
{
	int rc;
	HANDLE handle;
	LPSTR name = NULL;
	rc = ConvertFromUnicode(CP_UTF8, 0, lpTimerName, -1, &name, 0, NULL, NULL);

	if (rc < 0)
		return NULL;

	handle = CreateWaitableTimerA(lpTimerAttributes, bManualReset, name);
	free(name);
	return handle;
}

HANDLE CreateWaitableTimerExA(LPSECURITY_ATTRIBUTES lpTimerAttributes, LPCSTR lpTimerName,
                              DWORD dwFlags, DWORD dwDesiredAccess)
{
	BOOL bManualReset = (dwFlags & CREATE_WAITABLE_TIMER_MANUAL_RESET) ? TRUE : FALSE;

	if (dwDesiredAccess != 0)
		WLog_WARN(TAG, "%s [%s] does not support dwDesiredAccess 0x%08" PRIx32, __FUNCTION__,
		          lpTimerName, dwDesiredAccess);

	return CreateWaitableTimerA(lpTimerAttributes, bManualReset, lpTimerName);
}

HANDLE CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES lpTimerAttributes, LPCWSTR lpTimerName,
                              DWORD dwFlags, DWORD dwDesiredAccess)
{
	int rc;
	HANDLE handle;
	LPSTR name = NULL;
	rc = ConvertFromUnicode(CP_UTF8, 0, lpTimerName, -1, &name, 0, NULL, NULL);

	if (rc < 0)
		return NULL;

	handle = CreateWaitableTimerExA(lpTimerAttributes, name, dwFlags, dwDesiredAccess);
	free(name);
	return handle;
}

BOOL SetWaitableTimer(HANDLE hTimer, const LARGE_INTEGER* lpDueTime, LONG lPeriod,
                      PTIMERAPCROUTINE pfnCompletionRoutine, LPVOID lpArgToCompletionRoutine,
                      BOOL fResume)
{
	ULONG Type;
	WINPR_HANDLE* Object;
	WINPR_TIMER* timer;
#if defined(WITH_POSIX_TIMER) || defined(__APPLE__) || defined(WITH_KQUEUE)
	LONGLONG seconds = 0;
	LONGLONG nanoseconds = 0;
#ifdef HAVE_SYS_TIMERFD_H
	int status = 0;
#endif /* HAVE_SYS_TIMERFD_H */
#endif /* WITH_POSIX_TIMER */

	if (!winpr_Handle_GetInfo(hTimer, &Type, &Object))
		return FALSE;

	if (Type != HANDLE_TYPE_TIMER)
		return FALSE;

	if (!lpDueTime)
		return FALSE;

	if (lPeriod < 0)
		return FALSE;

	if (fResume)
	{
		WLog_ERR(TAG, "%s does not support fResume", __FUNCTION__);
		return FALSE;
	}

	timer = (WINPR_TIMER*)Object;
	timer->lPeriod = lPeriod; /* milliseconds */
	timer->pfnCompletionRoutine = pfnCompletionRoutine;
	timer->lpArgToCompletionRoutine = lpArgToCompletionRoutine;

	if (!timer->bInit)
	{
    WLog_VRB(TAG, "%s: Initializing timer %s", __FUNCTION__, timer->name);
		if (InitializeWaitableTimer(timer) < 0)
			return FALSE;
    WLog_VRB(TAG, "%s: Initialization complete for timer %s", __FUNCTION__, timer->name);
	}

#if defined(WITH_POSIX_TIMER) || defined(WITH_KQUEUE)
	ZeroMemory(&(timer->timeout), sizeof(struct itimerspec));

	if (lpDueTime->QuadPart < 0)
	{
		LONGLONG due = lpDueTime->QuadPart * (-1);
		/* due time is in 100 nanosecond intervals */
		seconds = (due / 10000000);
		nanoseconds = ((due % 10000000) * 100);
	}
	else if (lpDueTime->QuadPart == 0)
	{
		seconds = nanoseconds = 0;
	}
	else
	{
		WLog_ERR(TAG, "absolute time not implemented");
		return FALSE;
	}

	if (lPeriod > 0)
	{
		timer->timeout.it_interval.tv_sec = (lPeriod / 1000);              /* seconds */
		timer->timeout.it_interval.tv_nsec = ((lPeriod % 1000) * 1000000); /* nanoseconds */
	}

	if (lpDueTime->QuadPart != 0)
	{
		timer->timeout.it_value.tv_sec = seconds;      /* seconds */
		timer->timeout.it_value.tv_nsec = nanoseconds; /* nanoseconds */
	}
	else
	{
		timer->timeout.it_value.tv_sec = timer->timeout.it_interval.tv_sec;   /* seconds */
		timer->timeout.it_value.tv_nsec = timer->timeout.it_interval.tv_nsec; /* nanoseconds */
	}
#endif

	if (!timer->pfnCompletionRoutine)
	{
    WLog_VRB(TAG, "%s: Completion Routine IS NOT Set for timer %s", __FUNCTION__, timer->name);
#ifdef HAVE_SYS_TIMERFD_H
		status = timerfd_settime(timer->fd, 0, &(timer->timeout), NULL);

		if (status)
		{
			WLog_ERR(TAG, "timerfd_settime failure: %d", status);
			return FALSE;
		}

#endif
#if defined(WITH_KQUEUE)
    uint64_t timeout_msec = (timer->timeout.it_value.tv_sec * 1000LL) + (timer->timeout.it_value.tv_nsec /1000000LL );
    WLog_VRB(TAG, "%s: Updating timeout for timer %s to %lu ms", __FUNCTION__, timer->name, timeout_msec);
    EV_SET(&timer->event, 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT , 0, timeout_msec, timer);
    kevent(timer->fd, &timer->event, 1, NULL, 0, NULL);
#endif
	}
	else
	{
    WLog_VRB(TAG, "%s: Completion Routine IS Set for timer %s", __FUNCTION__, timer->name);
#ifdef WITH_POSIX_TIMER
		if ((timer_settime(timer->tid, 0, &(timer->timeout), NULL)) != 0)
		{
			WLog_ERR(TAG, "timer_settime");
			return FALSE;
		}
#endif
#if defined(WITH_KQUEUE)
    uint64_t timeout_msec = (timer->timeout.it_value.tv_sec * 1000LL) + (timer->timeout.it_value.tv_nsec /1000000LL );
    WLog_VRB(TAG, "%s: Updating timeout for timer %s to %lu ms", __FUNCTION__, timer->name, timeout_msec);
    EV_SET(&timer->event, 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT , 0, timeout_msec, timer);
    kevent(timer->fd, &timer->event, 1, NULL, 0, NULL);
#endif
	}

#if defined(__APPLE__)

	if (lpDueTime->QuadPart < 0)
	{
		LONGLONG due = lpDueTime->QuadPart * (-1);
		/* due time is in 100 nanosecond intervals */
		seconds = (due / 10000000);
		nanoseconds = due * 100;
	}
	else if (lpDueTime->QuadPart == 0)
	{
		seconds = nanoseconds = 0;
	}
	else
	{
		WLog_ERR(TAG, "absolute time not implemented");
		return FALSE;
	}

	{
		/* Clean out old data from FD */
		BYTE buffer[32];

		while (read(timer->fd, buffer, sizeof(buffer)) > 0)
			;
	}

	{
		if (timer->running)
			dispatch_suspend(timer->source);

		dispatch_time_t start = dispatch_time(DISPATCH_TIME_NOW, nanoseconds);
		uint64_t interval = DISPATCH_TIME_FOREVER;

		if (lPeriod > 0)
			interval = lPeriod * 1000000;

		dispatch_source_set_timer(timer->source, start, interval, 0);
		dispatch_resume(timer->source);
		timer->running = TRUE;
	}

#endif
	return TRUE;
}

BOOL SetWaitableTimerEx(HANDLE hTimer, const LARGE_INTEGER* lpDueTime, LONG lPeriod,
                        PTIMERAPCROUTINE pfnCompletionRoutine, LPVOID lpArgToCompletionRoutine,
                        PREASON_CONTEXT WakeContext, ULONG TolerableDelay)
{
	return SetWaitableTimer(hTimer, lpDueTime, lPeriod, pfnCompletionRoutine,
	                        lpArgToCompletionRoutine, FALSE);
}

HANDLE OpenWaitableTimerA(DWORD dwDesiredAccess, BOOL bInheritHandle, LPCSTR lpTimerName)
{
	/* TODO: Implement */
	WLog_ERR(TAG, "%s not implemented", __FUNCTION__);
	return NULL;
}

HANDLE OpenWaitableTimerW(DWORD dwDesiredAccess, BOOL bInheritHandle, LPCWSTR lpTimerName)
{
	/* TODO: Implement */
	WLog_ERR(TAG, "%s not implemented", __FUNCTION__);
	return NULL;
}

BOOL CancelWaitableTimer(HANDLE hTimer)
{
	ULONG Type;
	WINPR_HANDLE* Object;
	WINPR_TIMER* timer;

	if (!winpr_Handle_GetInfo(hTimer, &Type, &Object))
	return FALSE;

	if (Type != HANDLE_TYPE_TIMER)
		return FALSE;

	timer = (WINPR_TIMER*)Object;
#if defined(__APPLE__)

	if (timer->running)
		dispatch_suspend(timer->source);

	timer->running = FALSE;
#endif
	return TRUE;
}

/**
 * Timer-Queue Timer
 */

/**
 * Design, Performance, and Optimization of Timer Strategies for Real-time ORBs:
 * http://www.cs.wustl.edu/~schmidt/Timer_Queue.html
 */

static void timespec_add_ms(struct timespec* tspec, UINT32 ms)
{
	UINT64 ns = tspec->tv_nsec + (ms * 1000000);
	tspec->tv_sec += (ns / 1000000000);
	tspec->tv_nsec = (ns % 1000000000);
}

static UINT64 timespec_to_ms(struct timespec* tspec)
{
	UINT64 ms;
	ms = tspec->tv_sec * 1000;
	ms += tspec->tv_nsec / 1000000;
	return ms;
}

static void timespec_gettimeofday(struct timespec* tspec)
{
	struct timeval tval;
	gettimeofday(&tval, NULL);
	tspec->tv_sec = tval.tv_sec;
	tspec->tv_nsec = tval.tv_usec * 1000;
}

static int timespec_compare(const struct timespec* tspec1, const struct timespec* tspec2)
{
	if (tspec1->tv_sec == tspec2->tv_sec)
		return (tspec1->tv_nsec - tspec2->tv_nsec);
	else
		return (tspec1->tv_sec - tspec2->tv_sec);
}

static void timespec_copy(struct timespec* dst, struct timespec* src)
{
	dst->tv_sec = src->tv_sec;
	dst->tv_nsec = src->tv_nsec;
}

static void InsertTimerQueueTimer(WINPR_TIMER_QUEUE_TIMER** pHead, WINPR_TIMER_QUEUE_TIMER* timer)
{
	WINPR_TIMER_QUEUE_TIMER* node;

	if (!(*pHead))
	{
		*pHead = timer;
		timer->next = NULL;
		return;
	}

	node = *pHead;

	while (node->next)
	{
		if (timespec_compare(&(timer->ExpirationTime), &(node->ExpirationTime)) > 0)
		{
			if (timespec_compare(&(timer->ExpirationTime), &(node->next->ExpirationTime)) < 0)
				break;
		}

		node = node->next;
	}

	if (node->next)
	{
		timer->next = node->next->next;
		node->next = timer;
	}
	else
	{
		node->next = timer;
		timer->next = NULL;
	}
}

static void RemoveTimerQueueTimer(WINPR_TIMER_QUEUE_TIMER** pHead, WINPR_TIMER_QUEUE_TIMER* timer)
{
	BOOL found = FALSE;
	WINPR_TIMER_QUEUE_TIMER* node;
	WINPR_TIMER_QUEUE_TIMER* prevNode;

	if (timer == *pHead)
	{
		*pHead = timer->next;
		timer->next = NULL;
		return;
	}

	node = *pHead;
	prevNode = NULL;

	while (node)
	{
		if (node == timer)
		{
			found = TRUE;
			break;
		}

		prevNode = node;
		node = node->next;
	}

	if (found)
	{
		if (prevNode)
		{
			prevNode->next = timer->next;
		}

		timer->next = NULL;
	}
}

static int FireExpiredTimerQueueTimers(WINPR_TIMER_QUEUE* timerQueue)
{
	struct timespec CurrentTime;
	WINPR_TIMER_QUEUE_TIMER* node;

	if (!timerQueue->activeHead)
		return 0;

	timespec_gettimeofday(&CurrentTime);
	node = timerQueue->activeHead;

	while (node)
	{
		if (timespec_compare(&CurrentTime, &(node->ExpirationTime)) >= 0)
		{
			node->Callback(node->Parameter, TRUE);
			node->FireCount++;
			timerQueue->activeHead = node->next;
			node->next = NULL;

			if (node->Period)
			{
				timespec_add_ms(&(node->ExpirationTime), node->Period);
				InsertTimerQueueTimer(&(timerQueue->activeHead), node);
			}
			else
			{
				InsertTimerQueueTimer(&(timerQueue->inactiveHead), node);
			}

			node = timerQueue->activeHead;
		}
		else
		{
			break;
		}
	}

	return 0;
}

static void* TimerQueueThread(void* arg)
{
	int status;
	struct timespec timeout;
	WINPR_TIMER_QUEUE* timerQueue = (WINPR_TIMER_QUEUE*)arg;

	while (1)
	{
		pthread_mutex_lock(&(timerQueue->cond_mutex));
		timespec_gettimeofday(&timeout);

		if (!timerQueue->activeHead)
		{
			timespec_add_ms(&timeout, 50);
		}
		else
		{
			if (timespec_compare(&timeout, &(timerQueue->activeHead->ExpirationTime)) < 0)
			{
				timespec_copy(&timeout, &(timerQueue->activeHead->ExpirationTime));
			}
		}

		status = pthread_cond_timedwait(&(timerQueue->cond), &(timerQueue->cond_mutex), &timeout);
		FireExpiredTimerQueueTimers(timerQueue);
		pthread_mutex_unlock(&(timerQueue->cond_mutex));

		if ((status != ETIMEDOUT) && (status != 0))
			break;

		if (timerQueue->bCancelled)
			break;
	}

	return NULL;
}

static int StartTimerQueueThread(WINPR_TIMER_QUEUE* timerQueue)
{
	pthread_cond_init(&(timerQueue->cond), NULL);
	pthread_mutex_init(&(timerQueue->cond_mutex), NULL);
	pthread_mutex_init(&(timerQueue->mutex), NULL);
	pthread_attr_init(&(timerQueue->attr));
	timerQueue->param.sched_priority = sched_get_priority_max(SCHED_FIFO);
	pthread_attr_setschedparam(&(timerQueue->attr), &(timerQueue->param));
	pthread_attr_setschedpolicy(&(timerQueue->attr), SCHED_FIFO);
	pthread_create(&(timerQueue->thread), &(timerQueue->attr), TimerQueueThread, timerQueue);
	return 0;
}

HANDLE CreateTimerQueue(void)
{
	HANDLE handle = NULL;
	WINPR_TIMER_QUEUE* timerQueue;
	timerQueue = (WINPR_TIMER_QUEUE*)calloc(1, sizeof(WINPR_TIMER_QUEUE));

	if (timerQueue)
	{
		WINPR_HANDLE_SET_TYPE_AND_MODE(timerQueue, HANDLE_TYPE_TIMER_QUEUE, WINPR_FD_READ);
		handle = (HANDLE)timerQueue;
		timerQueue->activeHead = NULL;
		timerQueue->inactiveHead = NULL;
		timerQueue->bCancelled = FALSE;
		StartTimerQueueThread(timerQueue);
	}

	return handle;
}

BOOL DeleteTimerQueueEx(HANDLE TimerQueue, HANDLE CompletionEvent)
{
	void* rvalue;
	WINPR_TIMER_QUEUE* timerQueue;
	WINPR_TIMER_QUEUE_TIMER* node;
	WINPR_TIMER_QUEUE_TIMER* nextNode;

	if (!TimerQueue)
		return FALSE;

	timerQueue = (WINPR_TIMER_QUEUE*)TimerQueue;
	/* Cancel and delete timer queue timers */
	pthread_mutex_lock(&(timerQueue->cond_mutex));
	timerQueue->bCancelled = TRUE;
	pthread_cond_signal(&(timerQueue->cond));
	pthread_mutex_unlock(&(timerQueue->cond_mutex));
	pthread_join(timerQueue->thread, &rvalue);
	/**
	 * Quote from MSDN regarding CompletionEvent:
	 * If this parameter is INVALID_HANDLE_VALUE, the function waits for
	 * all callback functions to complete before returning.
	 * If this parameter is NULL, the function marks the timer for
	 * deletion and returns immediately.
	 *
	 * Note: The current WinPR implementation implicitly waits for any
	 * callback functions to complete (see pthread_join above)
	 */
	{
		/* Move all active timers to the inactive timer list */
		node = timerQueue->activeHead;

		while (node)
		{
			InsertTimerQueueTimer(&(timerQueue->inactiveHead), node);
			node = node->next;
		}

		timerQueue->activeHead = NULL;
		/* Once all timers are inactive, free them */
		node = timerQueue->inactiveHead;

		while (node)
		{
			nextNode = node->next;
			free(node);
			node = nextNode;
		}

		timerQueue->inactiveHead = NULL;
	}
	/* Delete timer queue */
	pthread_cond_destroy(&(timerQueue->cond));
	pthread_mutex_destroy(&(timerQueue->cond_mutex));
	pthread_mutex_destroy(&(timerQueue->mutex));
	pthread_attr_destroy(&(timerQueue->attr));
	free(timerQueue);

	if (CompletionEvent && (CompletionEvent != INVALID_HANDLE_VALUE))
		SetEvent(CompletionEvent);

	return TRUE;
}

BOOL DeleteTimerQueue(HANDLE TimerQueue)
{
	return DeleteTimerQueueEx(TimerQueue, NULL);
}

BOOL CreateTimerQueueTimer(PHANDLE phNewTimer, HANDLE TimerQueue, WAITORTIMERCALLBACK Callback,
                           PVOID Parameter, DWORD DueTime, DWORD Period, ULONG Flags)
{
	struct timespec CurrentTime;
	WINPR_TIMER_QUEUE* timerQueue;
	WINPR_TIMER_QUEUE_TIMER* timer;

	if (!TimerQueue)
		return FALSE;

	timespec_gettimeofday(&CurrentTime);
	timerQueue = (WINPR_TIMER_QUEUE*)TimerQueue;
	timer = (WINPR_TIMER_QUEUE_TIMER*)malloc(sizeof(WINPR_TIMER_QUEUE_TIMER));

	if (!timer)
		return FALSE;

	WINPR_HANDLE_SET_TYPE_AND_MODE(timer, HANDLE_TYPE_TIMER_QUEUE_TIMER, WINPR_FD_READ);
	*((UINT_PTR*)phNewTimer) = (UINT_PTR)(HANDLE)timer;
	timespec_copy(&(timer->StartTime), &CurrentTime);
	timespec_add_ms(&(timer->StartTime), DueTime);
	timespec_copy(&(timer->ExpirationTime), &(timer->StartTime));
	timer->Flags = Flags;
	timer->DueTime = DueTime;
	timer->Period = Period;
	timer->Callback = Callback;
	timer->Parameter = Parameter;
	timer->timerQueue = (WINPR_TIMER_QUEUE*)TimerQueue;
	timer->FireCount = 0;
	timer->next = NULL;
	pthread_mutex_lock(&(timerQueue->cond_mutex));
	InsertTimerQueueTimer(&(timerQueue->activeHead), timer);
	pthread_cond_signal(&(timerQueue->cond));
	pthread_mutex_unlock(&(timerQueue->cond_mutex));
	return TRUE;
}

BOOL ChangeTimerQueueTimer(HANDLE TimerQueue, HANDLE Timer, ULONG DueTime, ULONG Period)
{
	struct timespec CurrentTime;
	WINPR_TIMER_QUEUE* timerQueue;
	WINPR_TIMER_QUEUE_TIMER* timer;

	if (!TimerQueue || !Timer)
		return FALSE;

	timespec_gettimeofday(&CurrentTime);
	timerQueue = (WINPR_TIMER_QUEUE*)TimerQueue;
	timer = (WINPR_TIMER_QUEUE_TIMER*)Timer;
	pthread_mutex_lock(&(timerQueue->cond_mutex));
	RemoveTimerQueueTimer(&(timerQueue->activeHead), timer);
	RemoveTimerQueueTimer(&(timerQueue->inactiveHead), timer);
	timer->DueTime = DueTime;
	timer->Period = Period;
	timer->next = NULL;
	timespec_copy(&(timer->StartTime), &CurrentTime);
	timespec_add_ms(&(timer->StartTime), DueTime);
	timespec_copy(&(timer->ExpirationTime), &(timer->StartTime));
	InsertTimerQueueTimer(&(timerQueue->activeHead), timer);
	pthread_cond_signal(&(timerQueue->cond));
	pthread_mutex_unlock(&(timerQueue->cond_mutex));
	return TRUE;
}

BOOL DeleteTimerQueueTimer(HANDLE TimerQueue, HANDLE Timer, HANDLE CompletionEvent)
{
	WINPR_TIMER_QUEUE* timerQueue;
	WINPR_TIMER_QUEUE_TIMER* timer;

	if (!TimerQueue || !Timer)
		return FALSE;

	timerQueue = (WINPR_TIMER_QUEUE*)TimerQueue;
	timer = (WINPR_TIMER_QUEUE_TIMER*)Timer;
	pthread_mutex_lock(&(timerQueue->cond_mutex));
	/**
	 * Quote from MSDN regarding CompletionEvent:
	 * If this parameter is INVALID_HANDLE_VALUE, the function waits for
	 * all callback functions to complete before returning.
	 * If this parameter is NULL, the function marks the timer for
	 * deletion and returns immediately.
	 *
	 * Note: The current WinPR implementation implicitly waits for any
	 * callback functions to complete (see cond_mutex usage)
	 */
	RemoveTimerQueueTimer(&(timerQueue->activeHead), timer);
	pthread_cond_signal(&(timerQueue->cond));
	pthread_mutex_unlock(&(timerQueue->cond_mutex));
	free(timer);

	if (CompletionEvent && (CompletionEvent != INVALID_HANDLE_VALUE))
		SetEvent(CompletionEvent);

	return TRUE;
}

#endif
