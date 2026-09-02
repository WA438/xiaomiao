/**
 * terminal.h — XiaoMiaoOS Terminal / Telnet Server
 * Remote command execution via Telnet (port 23) + WebSocket
 * SD card file ops, HTTP download, system commands
 */
#ifndef TERMINAL_H
#define TERMINAL_H

#include "config.h"
#include <WiFi.h>
#include <SD.h>
#include <HTTPClient.h>

// Telnet server
void term_telnet_begin();
void term_telnet_stop();
void term_telnet_loop();  // call in main loop, non-blocking

// Command processor (returns output string)
String term_exec(const String& cmd);

#endif