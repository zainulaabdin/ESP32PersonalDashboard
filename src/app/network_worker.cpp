#include "network_worker.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <string.h>
#include "wake_word.h"

// Small fixed-capacity queue - bounded number of distinct job TYPES this
// app will ever have (ASK/USER_REFRESH/BUS/CALENDAR/WEATHER, plus real
// headroom for multiple queued ASKs since those are never deduplicated,
// requirement #9). No dynamic allocation needed for something this size.
#define NET_QUEUE_CAPACITY 16

struct QueuedJob
{
    NetworkJobType type;
    char jobName[16];
    NetworkJobFn fn;
    bool isPeriodic;
};

static QueuedJob queueSlots[NET_QUEUE_CAPACITY];
static int queueCount = 0;
static SemaphoreHandle_t queueMutex = NULL;   // protects queueSlots/queueCount
static SemaphoreHandle_t queueNotify = NULL;  // counting semaphore - worker blocks on this when idle, no polling
static TaskHandle_t workerTaskHandle = NULL;
// Job currently executing - read by status_led.cpp.
static char runningJobName[16] = "";
static volatile bool jobRunning = false;

// Single worker task's own stack - created ONCE at boot, never destroyed/
// recreated per job (unlike the old per-fetch xTaskCreatePinnedToCore
// pattern). 20480 matches the largest of the old per-tab task stacks
// (bus.cpp's/today.cpp's own sizing, which already accounted for mbedTLS's
// real handshake needs) - safe to reuse since only one job ever runs at a
// time on this one stack, so it only needs to be as big as the single
// largest job actually requires, not the sum of all of them.
// Measured peaks (per-job high-water log): bus/weather ~5KB, calendar
// ~9KB, Ask ~8.5KB. 14KB keeps ~5KB margin and frees 6KB of internal RAM
// for the wake word listener.
#define WORKER_STACK_SIZE 14336

static void removeQueuedJobAt(int index)
{
    for (int i = index; i < queueCount - 1; i++)
        queueSlots[i] = queueSlots[i + 1];
    queueCount--;
}

// Picks and removes the highest-priority job currently queued (ties broken
// by insertion order - the queue is small and scanned linearly, real
// simplicity over a heap-based priority queue that this job count doesn't
// need). Caller must hold queueMutex.
static bool popHighestPriorityJob(QueuedJob *out)
{
    if (queueCount == 0)
        return false;
    int bestIndex = 0;
    for (int i = 1; i < queueCount; i++)
    {
        if ((int)queueSlots[i].type > (int)queueSlots[bestIndex].type)
            bestIndex = i;
    }
    *out = queueSlots[bestIndex];
    removeQueuedJobAt(bestIndex);
    return true;
}

static void logWaitingJobs()
{
    // Requirement's own example log format: "QUEUE: job=ASK waiting" for
    // whatever is still queued behind the job that just started running.
    for (int i = 0; i < queueCount; i++)
        Serial.printf("QUEUE: job=%s waiting\n", queueSlots[i].jobName);
}

static void networkWorkerTask(void *)
{
    for (;;)
    {
        // Blocks here (no polling, no CPU spent) until networkWorkerSubmit()
        // gives this semaphore - portMAX_DELAY is safe specifically because
        // this task has nothing else to do while idle; it is the only
        // consumer of this semaphore.
        xSemaphoreTake(queueNotify, portMAX_DELAY);

        QueuedJob job;
        bool got;
        {
            xSemaphoreTake(queueMutex, portMAX_DELAY);
            got = popHighestPriorityJob(&job);
            xSemaphoreGive(queueMutex);
        }
        if (!got)
            continue; // spurious wake (shouldn't happen - notify count always matches a real submit) - just re-block

        Serial.printf("QUEUE: running job=%s\n", job.jobName);
        {
            xSemaphoreTake(queueMutex, portMAX_DELAY);
            logWaitingJobs();
            xSemaphoreGive(queueMutex);
        }

        strlcpy(runningJobName, job.jobName, sizeof(runningJobName));
        jobRunning = true;
        // The wake word listener holds ~16KB of internal RAM (I2S DMA + task
        // stack); TLS needs it (real log: "esp-aes: Failed to allocate
        // memory" on the OTA check). It stays off until the job finishes.
        wakeWordPauseForNetwork();
        job.fn(); // the actual existing fetchBusWork()/fetchAgendaWork()/askWithAudio()-wrapping function, unchanged

        jobRunning = false;
        // Measuring before shrinking WORKER_STACK_SIZE (internal RAM).
        Serial.printf("QUEUE: job=%s stack high-water: %u bytes unused of %u\n", job.jobName,
                      (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned)WORKER_STACK_SIZE);
        Serial.printf("QUEUE: completed job=%s\n", job.jobName);
    }
}

void networkWorkerInit()
{
    queueMutex = xSemaphoreCreateMutex();
    queueNotify = xSemaphoreCreateCounting(NET_QUEUE_CAPACITY, 0);
    // Must stay in internal RAM - a PSRAM-backed stack
    // (xTaskCreateStaticPinnedToCore() + ps_malloc()) trips FreeRTOS's
    // xPortcheckValidStackMem assertion on this chip and boot-loops.
    xTaskCreatePinnedToCore(networkWorkerTask, "netWorker", WORKER_STACK_SIZE, NULL, 1, &workerTaskHandle, 0);
}

void networkWorkerSubmit(NetworkJobType type, const char *jobName, NetworkJobFn fn, bool isPeriodic)
{
    if (!queueMutex || !queueNotify)
        return; // called before networkWorkerInit() - shouldn't happen past setup()

    xSemaphoreTake(queueMutex, portMAX_DELAY);

    if (isPeriodic)
    {
        // Requirement #8: only one pending periodic job (BUS/CALENDAR/
        // WEATHER) of a given type at a time - if one of this exact type is
        // already queued (not yet started), don't add a second. A job
        // that's already RUNNING isn't in queueSlots any more (popped off
        // in networkWorkerTask above), so this only catches genuine
        // duplicate-while-still-pending, not "don't refresh again after
        // the last refresh already finished".
        for (int i = 0; i < queueCount; i++)
        {
            if (queueSlots[i].type == type && queueSlots[i].isPeriodic)
            {
                Serial.printf("QUEUE: duplicate pending job=%s ignored\n", jobName);
                xSemaphoreGive(queueMutex);
                return;
            }
        }
    }

    if (queueCount >= NET_QUEUE_CAPACITY)
    {
        // Real backstop, not expected in practice at this app's scale (ASK
        // is the only always-queued type, and a human can't tap fast enough
        // to fill 16 slots) - fail loud rather than silently drop or
        // overflow the array.
        Serial.printf("QUEUE: FULL - job=%s dropped\n", jobName);
        xSemaphoreGive(queueMutex);
        return;
    }

    QueuedJob &slot = queueSlots[queueCount++];
    slot.type = type;
    strlcpy(slot.jobName, jobName, sizeof(slot.jobName));
    slot.fn = fn;
    slot.isPeriodic = isPeriodic;

    Serial.printf("QUEUE: added job=%s priority=%d\n", jobName, (int)type);

    xSemaphoreGive(queueMutex);
    xSemaphoreGive(queueNotify); // wake the worker task if it was idle
}

const char *networkWorkerRunningJob()
{
    return jobRunning ? runningJobName : nullptr;
}
