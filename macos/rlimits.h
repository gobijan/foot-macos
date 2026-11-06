#pragma once

/*
 * The open files limit (RLIMIT_NOFILE): footd raises its own (each
 * session takes a dozen file descriptors, and applications start with
 * a limit of 256), and what it starts gets the usual one again.
 */

/* Raises the soft limit, up to the hard one */
void rlimits_raise_open_files(void);

/* In a child, before exec(): the limit as it was (if it was raised) */
void rlimits_restore_open_files(void);
