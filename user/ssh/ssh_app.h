#pragma once

/*
 * M5OS SSH-2 Client (RFC 4253 / 4252 / 4254 / 8731)
 *
 * Key Exchange : curve25519-sha256, curve25519-sha256@libssh.org
 * Host Key     : ssh-ed25519, ecdsa-sha2-nistp256, rsa-sha2-256, rsa-sha2-512, ssh-rsa
 * Cipher       : aes128-ctr
 * MAC          : hmac-sha2-256
 * Auth         : password, keyboard-interactive
 */

/* Parse and run `ssh` CLI command:
 *   ssh [user@]host [-p port] [-pw password] [remote_command...]
 *   ssh (no args: opens interactive TUI prompt)
 */
int ssh_cmd_main(int argc, char **argv);

/* Run interactive TUI SSH launcher (for `run ssh` or `ssh` with no arguments). */
void ssh_app_run(void);
