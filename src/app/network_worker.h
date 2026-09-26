#pragma once
#include <stdint.h>

// Single serialized network worker with a priority queue - replaces the
// old architecture where bus.cpp/today.cpp/ask.cpp each spawned their own
// FreeRTOS task per fetch (xTaskCreatePinnedToCore, 20480/12288-byte
// stacks). That design meant up to three separate task stacks competing
// for the ESP32's small internal heap at once, which real serial evidence
// showed could exhaust it (as little as ~23KB free), causing task
// creation itself to fail and the UI to get stuck - see git history/
// ask.cpp's old attemptCreateAskTask() comment for the full story. One
// worker task, created once at boot, removes that failure mode entirely:
// there is only ever one network-job stack alive, period.
//
// Jobs are plain function pointers (the exact existing fetchBusWork()/
// fetchAgendaWork()/askWithAudio()-wrapping functions, unchanged) so this
// is a queuing/scheduling layer on top of already-working code, not a
// rewrite of it (see each *.cpp's own networkWorkerJob wrapper).
enum NetworkJobType
{
    NET_JOB_ASK = 100,
    NET_JOB_USER_REFRESH = 50,
    // WEATHER outranks BUS/CALENDAR (was 10, lowest of the three - it used
    // to run LAST at boot), per explicit request that NEA weather load
    // before bus/calendar - still below ASK/USER_REFRESH so a live user
    // action isn't preempted by a periodic boot fetch.
    NET_JOB_WEATHER = 35,
    NET_JOB_BUS = 30,
    NET_JOB_CALENDAR = 20,
    NET_JOB_OTA = 10, // background firmware check - never ahead of live data
};

typedef void (*NetworkJobFn)();

void networkWorkerInit(); // call once from setup(), before any job can be submitted

// Submits a job to run on the worker task. jobName is a short label used
// only for the "QUEUE: ..." log lines (e.g. "ASK", "BUS", "CALENDAR").
// isPeriodic marks BUS/CALENDAR/WEATHER-style jobs that should never have
// more than one pending copy in the queue at once (requirement #8) - ASK/
// USER_REFRESH pass isPeriodic=false and are never deduplicated
// (requirement #9), so tapping the mic again mid-answer, or refreshing
// again right after a refresh, always queues a real new job rather than
// being silently dropped.
void networkWorkerSubmit(NetworkJobType type, const char *jobName, NetworkJobFn fn, bool isPeriodic);

// Name of the job currently executing ("BUS", "USER_REFRESH(calendar)", ...),
// or nullptr when the worker is idle.
const char *networkWorkerRunningJob();
