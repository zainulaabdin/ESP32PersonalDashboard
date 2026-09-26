#pragma once

// "Jarvis" wake word - see wake_word.cpp.
bool wakeWordInit(); // loads the model and starts the listener; false if the model partition is missing
bool wakeWordAvailable();
// Call every loop(): (re)starts the listener task when the audio path is free.
void wakeWordTick();
bool wakeWordEnabled();
void wakeWordSetEnabled(bool on); // persisted
// Call before recording/playback: stops the listener and waits until it
// has let go of the codec. handOver=true (recording) leaves the mic stream
// installed for micCaptureRecord() to take over; false (speaker) closes it.
void wakeWordRelease(bool handOver = false);
// Network worker, before each job: waits until the listener has stopped
// (it stays off while a job runs) so TLS gets the internal RAM.
void wakeWordPauseForNetwork();
// True once per detection (askTick() starts a recording).
bool wakeWordTakeDetection();
