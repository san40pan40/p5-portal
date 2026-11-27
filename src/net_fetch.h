#pragma once
/*
 * NetFetch — HTTPS jobs in one background FreeRTOS task, so loop() (web panel, DNS) keeps
 * running while a TLS session is open. One job (one TLS session) at a time; the task is
 * created per job and deletes itself when done. CA check: the built-in Mozilla bundle.
 *
 * Jobs: firmware version check (GitHub "latest release" redirect) and firmware install
 * (release asset firmware.bin -> Update). Later: list downloads (same task, new Job value).
 * The web layer starts a job and polls status(); texts for errors live in the web layer (i18n).
 */
#include <Arduino.h>

namespace NetFetch {

enum Job : uint8_t { JOB_NONE, JOB_FW_CHECK, JOB_FW_INSTALL };
enum State : uint8_t { IDLE, RUNNING, DONE, FAILED };
enum Err : uint8_t {
  E_NONE,
  E_BUSY,      // another job / an OTA upload is running
  E_HEAP,      // not enough internal RAM for TLS (code = largest free block, KB)
  E_TASK,      // could not create the task
  E_CONNECT,   // connection / TLS failed (code = HTTPClient error, detail = its text)
  E_HTTP,      // unexpected HTTP status (code)
  E_SIZE,      // the server did not send the file size
  E_TOO_BIG,   // the file does not fit the OTA partition (code = size, KB)
  E_TIMEOUT,   // download stalled or the connection dropped
  E_FLASH,     // Update error (detail = Update.errorString())
};

struct Status {
  Job     job      = JOB_NONE;
  State   state    = IDLE;
  Err     err      = E_NONE;
  uint8_t progress = 0;        // install: 0..100
  int     code     = 0;        // see Err
  char    latest[40] = "";     // check: latest release tag, "" = no releases yet
  char    detail[48] = "";
};

Err    start(Job job);   // E_NONE = started; otherwise the reason (nothing started)
bool   busy();
Status status();         // snapshot; text fields are final once state != RUNNING

}  // namespace NetFetch
