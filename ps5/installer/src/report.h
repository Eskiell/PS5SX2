/* PS5SX2 Installer: the report of one PS5SX2 session, like the settings page's "Download logs". */
#pragma once

#include "util.h"

#include <sys/types.h>
#include <time.h>

typedef struct {
  pid_t pid;
  time_t started;      /* pid.txt's time */
  int watched;         /* 1 = the logger saw it end; 0 = found afterwards */
  const char *suffix;  /* "" = the logs of the last session; ".1" = the one before */
  /* filled in by report_build: */
  char label[160];     /* "Test build 1 · vk-285-112" */
  char build[80];      /* "vk-285-112" */
  char game[256];      /* "Black (USA).iso" */
  char end[48];        /* ok, crash, gpu-hang, no-start, no-game (+note) */
  char end_line[400];
} session_info;

/* Builds the report (addresses and the settings page's token removed); 0 on success, -1 (nothing to send) if
 * that couldn't be done. */
int report_build(session_info *s, sbuf *out);

/* Masks IPv4/IPv6/MAC addresses and the given secret in place (secret may be NULL); -1 if it couldn't. */
int report_redact(sbuf *text, const char *secret);
