#pragma once

#include "eventloop_timer_utilities.h"
#include "stdbool.h"
#include <applibs/eventloop.h>

typedef struct {
	void (*handler)(EventLoopTimer* timer);
	struct timespec period;
	EventLoopTimer* eventLoopTimer;
	const char* name;
} Timer;

void StartTimerSet(Timer* timerSet[], size_t timerCount);
void StopTimerSet(void);
bool StartTimer(Timer* timer);
void StopTimer(Timer* timer);
EventLoop* GetTimerEventLoop(void);
void StopTimerEventLoop(void);
bool ChangeTimer(Timer* timer, const struct timespec* period);
bool SetOneShotTimer(Timer* timer, const struct timespec* delay);