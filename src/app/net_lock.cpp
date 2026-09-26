#include "net_lock.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <WiFi.h>
#include <Arduino.h>

// A binary semaphore, not a real FreeRTOS mutex - deliberately, so
// netLockForceRelease() (called from a watchdog on a task that isn't the
// current holder) is well-defined instead of corrupting a mutex's
// ownership/recursion tracking.
static SemaphoreHandle_t netMutex = NULL;

void netLockInit()
{
    netMutex = xSemaphoreCreateBinary();
    if (netMutex)
        xSemaphoreGive(netMutex); // starts "available"
}

NetLockGuard::NetLockGuard(uint32_t timeoutMs)
{
    ok = netMutex && xSemaphoreTake(netMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

NetLockGuard::~NetLockGuard()
{
    if (ok)
        xSemaphoreGive(netMutex);
}

void netLockForceRelease()
{
    if (netMutex)
        xSemaphoreGive(netMutex); // no-op if already available - safe
}

bool wifiSettled()
{
    static unsigned long connectedSinceMs = 0;
    if (WiFi.status() != WL_CONNECTED)
    {
        connectedSinceMs = 0;
        return false;
    }
    if (connectedSinceMs == 0)
        connectedSinceMs = millis();
    return millis() - connectedSinceMs > 3000;
}
