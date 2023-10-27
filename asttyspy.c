/*
 * AsTTYSpy: Virtual TDD/TTY for Asterisk
 *
 * Copyright (C) 2022, Naveen Albert
 *
 * Naveen Albert <asterisk@phreaknet.org>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * 		http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*! \file
 *
 * \brief AsTTYSpy: Virtual TDD/TTY for Asterisk
 *
 * \author Naveen Albert <asterisk@phreaknet.org>
 */

/*
 * This program is a simple utility that allows you to turn any
 * terminal into a TTY/TDD. You can invoke the utility on any
 * active Asterisk channel. The channel on which to invoke AsTTYSpy
 * should be the channel to which you want to bridge, i.e. NOT
 * the channel on your side of the conversation, but the channel
 * on the other side. This utility will send the text you type
 * onto that channel as Baudot code, as well as relay any Baudot
 * code received on the target channel to text that will appear
 * in realtime on your terminal.
 * Because this utility can be used with any arbitrary channel,
 * you can use it to arbitrarily receive TTY data from any channel
 * and send TTY data to any channel, hence the name "AsTTYSpy".
 *
 * Compilation Instructions:
 * This program needs to be statically compiled with CAMI.
 * The latest source for CAMI can be downloaded from: https://github.com/InterLinked1/cami
 * The following files are required (with this folder hierarchy):
 * - cami.c
 * - include/cami.h
 * - include/cami_actions.h
 * (simpleami.c and Makefile from CAMI are not needed)
 *
 * Your folder hierarchy should this look like this:
 * - include/cami.h (from CAMI)
 * - include/cami_actions.h (from CAMI)
 * - cami.c (from CAMI)
 * - asttyspy.c (from AsTTYSpy)
 * - Makefile (from AsTTYSpy)
 *
 * To compile, simply run "make".
 *
 * Program Dependencies:
 * - CAMI:    https://github.com/InterLinked1/cami
 * - app_tdd: https://github.com/dgorski/app_tdd
 * - Asterisk manager.conf configuration:
 * -- Must have an AMI user with sufficient read/write permissions (call read/write).
 * -- The TddRxMsg event is essential to proper operation of this program, as well as being able to
 *    send TddTx actions.
 * -- The Newchannel and Hangup events are not strictly required, but auto refreshing of available channel selections
 *    will not work without these events. The DeviceStateChange may also be used instead. Alternately, if these
 *    events are not available for the AMI user used, you can specify -r to force a refresh every second.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/types.h>
#include <poll.h>
#include <termios.h>
#include <sys/socket.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <assert.h>

#include <cami/cami.h>
#include <cami/cami_actions.h>

#define TTY_MENU_OPTS "ESC +" \
	" [H] Help" \
	" [Q] Quit" \
	" [1] Dial Number" \
	" [2] Hangup" \
	" [4] Send Greeting" \
	" [8] Clear Screen" \
	"\n"

#define TTY_RX_OPTIONS "b(1)s"
#define TERM_CLEAR "\e[1;1H\e[2J"
#define KEY_ESCAPE 27

static pthread_mutex_t ttymutex = PTHREAD_MUTEX_INITIALIZER;
static char ttychan[256] = "";
static struct termios origterm, ttyterm;

/* Internal flags */
static int new_channel = 0;
static int our_turn = 0;
static int tty_active = 0;

/* Options */
static int always_refresh = 0;

/*! \brief Callback function executing asynchronously when new events are available */
static void ami_callback(struct ami_event *event)
{
	const char *msg, *channel, *eventname = ami_keyvalue(event, "Event");

	if (tty_active == 1 && (!strcmp(eventname, "Newchannel") || !strcmp(eventname, "Hangup") || !strcmp(eventname, "DeviceStateChange"))) {
		new_channel = 1; /* Keep track of any changes in the channels that exist. */
		goto cleanup;
	} else if (tty_active < 2) {
		goto cleanup; /* TTY isn't even active yet */
	} else if (strcmp(eventname, "TddRxMsg")) {
		goto cleanup; /* Don't care about non-TTY stuff */
	}

	channel = ami_keyvalue(event, "Channel");
	if (strcmp(channel, ttychan)) {
		goto cleanup; /* Not our channel */
	}

	/* Okay, this is actually for us. */
	msg = ami_keyvalue(event, "Message");
	pthread_mutex_lock(&ttymutex);
	if (our_turn) {
		printf("\nTTY: "); /* We changed who was typing. */
		our_turn = 0;
	}
	if (!strcmp(msg, "\\n")) { /* Convert text '\n' to actual newline */
		printf("\n");
	} else {
		char *msgdup = strdup(msg);
		if (msgdup) {
			char *c = msgdup;
			/* Replace _ with space */
			while (*c) {
				if (*c == '_') {
					*c = ' ';
				}
				c++;
			}
			printf("%s", msgdup);
			free(msgdup);
		}
	}
	fflush(stdout);
	pthread_mutex_unlock(&ttymutex);

cleanup:
	ami_event_free(event); /* Free event when done with it */
}

static void simple_disconnect_callback(void)
{
	/* Start with a newline, since we don't know where we were. */
	fprintf(stderr, "\nAMI was forcibly disconnected...\n");
	exit(EXIT_FAILURE);
}

static int wait_for_input(int timeout)
{
	struct pollfd pfd;
	int res;

	/* Wait for input. */
	pfd.fd = STDIN_FILENO;
	pfd.events = POLLIN;

	for (;;) {
		res = poll(&pfd, 1, timeout);
		if (res < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		} else if (pfd.revents) {
			return 1;
		} else {
			return 0;
		}
	}
	return -1;
}

#define IS_DTMF(x) (isdigit(x) || (x >= 'A' && x <= 'D') || x == '*' || x == '#')

static int send_dtmf(char digit)
{
	int res = ami_action_response_result(ami_action("PlayDTMF", "Channel:%s\r\nDigit:%c", ttychan, digit));
	return res;
}

static int send_msg(const char *typed)
{
	int res;
	char *tmp, *ttymsg = strdup(typed);

	if (!ttymsg) {
		return -1;
	}

	pthread_mutex_lock(&ttymutex);

	/* Replace spaces with _ for AMI, since it ignores whitespace. */
	tmp = ttymsg;
	while (*tmp) {
		if (*tmp == ' ') {
			*tmp = '_';
		}
		tmp++;
	}

	res = ami_action_response_result(ami_action("TddTx", "Channel:%s\r\nMessage:%s", ttychan, ttymsg));
	if (!res && !our_turn) {
		printf("\nCA : "); /* We changed who was typing. */
		our_turn = 1;
	}

	printf("%s", typed); /* Echo original input */
	pthread_mutex_unlock(&ttymutex);

	if (res) {
		fprintf(stderr, "\n*** CALL DISCONNECTED ***\n");
	} else {
		fflush(stdout);
	}

	free(ttymsg);
	return res;
}

static int handle_input(void)
{
	struct pollfd pfd;
	int res;
	int esc_mode, dtmf_mode = 0, got_escape = 0;
	char dialnum[64];

	/* Wait for input. */
	pfd.fd = STDIN_FILENO;
	pfd.events = POLLIN;

	for (;;) {
		/* This thread will block forever on input. */
		res = poll(&pfd, 1, -1);
		if (res < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		} else if (pfd.revents) {
			/* Got some input. */
			char tmpbuf[2];
			int num_read = read(STDIN_FILENO, tmpbuf, 1); /* Only read one char. */

			if (num_read < 1) {
				break; /* Disconnect */
			}

			esc_mode = 0;
			if (tmpbuf[0] == KEY_ESCAPE) {
				got_escape = 1;
				continue;
			} else if (got_escape) {
				got_escape = 0;
				esc_mode = 1;
			}

			/* Control actions */
			if (esc_mode) {
				int digits_read;
				char *current_digit = dialnum;
				switch (tmpbuf[0]) {
					case 'q':
					case 'Q': /* Quit */
						return -1;
					case 'h':
					case 'H':
						printf("\n" TTY_MENU_OPTS);
						fflush(stdout);
						break;
					case 'd': /* Toggle DTMF mode */
						dtmf_mode = dtmf_mode ? 0 : 1;
						break;
					/* Some Ultratec CTRL+# options */
					case '1': /* Dial number */
						/* Read number */
						printf("\nNBR: ");
						fflush(stdout);
						tcsetattr(STDIN_FILENO, TCSANOW, &origterm); /* Restore the original term settings */
						digits_read = read(STDIN_FILENO, dialnum, sizeof(dialnum)); /* Read numbering with buffering and echo */
						tcsetattr(STDIN_FILENO, TCSANOW, &ttyterm); /* Disable input buffering again. */
						if (digits_read < 1) {
							break; /* Disconnect */
						}
						/* Send number as DTMF */
						while (*current_digit) {
							if (IS_DTMF(*current_digit)) {
								if (send_dtmf(*current_digit)) {
									return -1;
								}
								usleep(100000); /* Wait 100ms */
							}
							current_digit++;
						}
						break;
					case '2': /* Disconnect (start over) */
						return 0;
					case '4': /* Send greeting memo */
						if (send_msg("HELLO GA")) {
							return -1;
						}
						break;
					case '8': /* Clear */
						printf(TERM_CLEAR);
						fflush(stdout);
						break;
					default:
						/* Ignore */
						break;
				}
				continue;
			}

			if (dtmf_mode && IS_DTMF(tmpbuf[0])) {
				/* Send DTMF, instead of TTY */
				if (send_dtmf(tmpbuf[0])) {
					return -1;
				}
				continue;
			}

			assert(num_read == 1);
			tmpbuf[1] = '\0'; /* Null terminate for string printing */
			if (send_msg(tmpbuf)) {
				return -1;
			}
		}
	}
	return -1;
}

static struct ami_response *print_channels(int *iptr)
{
	int i = *iptr;
	struct ami_response *resp;

	resp = ami_action_show_channels();
	if (!resp) {
		fprintf(stderr, "Failed to get channel list\n");
		return NULL;
	}
	/* Got a response to our action */
#define AMI_CHAN_FORMAT_HDR "%4s | %-40s | %8s | %15s | %15s\n"
#define AMI_CHAN_FORMAT_MSG "%4d | %-40s | %8s | %15s | %15s\n"

	/* The first "event" is simply the fields in the response itself (so ignore it). */
	/* The last event is simply "CoreShowChannelsComplete", for this action response (so ignore it). */
	printf("Channels: %d\n", resp->size - 2);
	printf(AMI_CHAN_FORMAT_HDR, "#", "Channel", "Duration", "Caller ID", "Called No.");
	for (i = 1; i < resp->size - 1; i++) {
		printf(AMI_CHAN_FORMAT_MSG,
			i,
			ami_keyvalue(resp->events[i], "Channel"),
			ami_keyvalue(resp->events[i], "Duration"),
			ami_keyvalue(resp->events[i], "CallerIDNum"),
			ami_keyvalue(resp->events[i], "ConnectedLineNum")
		);
	}

#undef AMI_CHAN_FORMAT_HDR
#undef AMI_CHAN_FORMAT_MSG
	*iptr = i;
	return resp;
}

static int get_channel(void)
{
	char channo[15];
	int chan_no;
	struct ami_response *resp = NULL;
	int i, res, invalid = 0;

	for (;;) {
		if (new_channel || always_refresh) {
			/* Print channels for first time, or update list if we got a new channel in the interim. */
			printf(TERM_CLEAR);
			printf("*** AsTTYSpy ***\n");
			printf("Target channel number should be the non-TTY side of the call\n");
			printf("i.e. the channel with which the TTY user is currently bridged\n");
			fflush(stdout);
			if (resp) {
				ami_resp_free(resp); /* Not needed anymore. */
			}
			resp = print_channels(&i);
			if (!resp) {
				return -1;
			}
			/* Prompt user for the channel number on which to setup a virtual TTY/TDD. */
			if (invalid) {
				printf("Invalid channel number: %s", channo); /* No trailing newline needed, since fgets captured one at the end, probably */
				if (!strchr(channo, '\n')) {
					printf("\n");
				}
				invalid = 0;
			}
			printf("=> Channel No.: ");
			fflush(stdout);
			new_channel = 0;
		}
		/* It would probably be more efficient to also wait on a pipe for new channels, but this works too. */
		res = wait_for_input(1000); /* Wait 2 seconds at a time. */
		if (res < 0) {
			res = -1;
			break;
		} else if (!res) {
			continue;
		}
		/* Okay, we got some input, so wait for the rest of it in a blocking manner. */
		if (!fgets(channo, sizeof(channo), stdin)) {
			res = -1;
			break;
		}
		if (!strcasecmp(channo, "q\n")) {
			res = -1;
			break;
		} else if (strcmp(channo, "\n")) {
			/* We got something (hopefully a valid channel number) */
			chan_no = atoi(channo);
			if (chan_no >= 1 && chan_no < i) {
				res = 0;
				break; /* Got a valid channel number */
			}
			invalid = 1;
		}
		new_channel = 1; /* User hit ENTER, just refresh the channel list and read again. */
	}

	/* Okay, we do have a valid  channel number. Determine which channel it is. */
	if (!res) {
		strncpy(ttychan, ami_keyvalue(resp->events[chan_no], "Channel"), sizeof(ttychan));
	}
	ami_resp_free(resp); /* Not needed anymore. */
	return res;
}

static void restore_term(int num)
{
	/* Be nice and restore the terminal to how it was before, before we exit. */
	pthread_mutex_lock(&ttymutex);
	tty_active = 0;
	tcsetattr(STDIN_FILENO, TCSANOW, &origterm); /* Restore the original term settings */
	pthread_mutex_unlock(&ttymutex);
	ami_disconnect();
	fprintf(stderr, "\nAsTTYSpy exiting...\n");
	exit(EXIT_FAILURE);
}

static int ttyspy(void)
{
	tcgetattr(STDIN_FILENO, &origterm);
	ttyterm = origterm;

	/* Set up the terminal */
	ttyterm.c_lflag &= ~ICANON; /* Disable canonical mode to disable input buffering. Needed so poll works correctly on STDIN_FILENO */
	signal(SIGINT, restore_term); /* Setup a signal handler for SIGINT, so we can restore the terminal. */
	tty_active = 1;
	tcsetattr(STDIN_FILENO, TCSANOW, &ttyterm); /* Apply changes */

	for (;;) {
		new_channel = 1;
		/* If a channel was not provided, prompt for one now. */
		if (!ttychan[0] && get_channel()) {
			break;
		}

		/* Enable TTY on the target channel. */
		if (ami_action_response_result(ami_action("TddRx", "Channel:%s\r\nOptions:%s", ttychan, TTY_RX_OPTIONS))) {
			/* This could be because TTY was already enabled on the channel (can't do it twice) */
			fprintf(stderr, "Failed to enable TTY on channel %s\n", ttychan);
			break;
		}

		/* Clear the screen. */
		printf(TERM_CLEAR);
		printf("*** AsTTYSpy ***\n");
		printf(TTY_MENU_OPTS);
		fflush(stdout);

		/* Set up the terminal */
		ttyterm.c_lflag &= ~ICANON; /* Disable canonical mode to disable input buffering. Needed for reading char by char. */
		ttyterm.c_lflag &= ~ECHO;	/* Disable echo */
		tty_active = 2; /* Get set, go! */
		tcsetattr(STDIN_FILENO, TCSANOW, &ttyterm); /* Apply changes */

		if (handle_input()) {
			break;
		}
		/* Do it on a new channel, so prompt for channel explicitly */
		ttychan[0] = '\0';
	}

	ami_disconnect();
	tcsetattr(STDIN_FILENO, TCSANOW, &origterm); /* Restore the original term settings */
	return 0;
}

static void show_help(void)
{
	printf("AsTTYSpy for Asterisk\n");
	printf(" -c <channel> Target channel with which to converse using this virtual TTY. If not provided, will prompt for selection.\n");
	printf(" -h           Show this help\n");
	printf(" -l           Asterisk AMI hostname. Default is localhost (127.0.0.1)\n");
	printf(" -p           Asterisk AMI password. By default, this will be autodetected for local connections if possible.\n");
	printf(" -r           Always refresh channel list during selection\n"); /* (rather than purely event driven) */
	printf(" -u           Asterisk AMI username.\n");
	printf("(C) 2022 Naveen Albert\n");
}

int main(int argc,char *argv[])
{
	char c;
	static const char *getopt_settings = "?c:hl:p:ru:";
	char ami_host[92] = "127.0.0.1"; /* Default to localhost */
	char ami_username[64] = "";
	char ami_password[64] = "";

	while ((c = getopt(argc, argv, getopt_settings)) != -1) {
		switch (c) {
		case 'c':
			strncpy(ttychan, optarg, sizeof(ttychan));
			break;
		case '?':
		case 'h':
			show_help();
			return 0;
		case 'l':
			strncpy(ami_host, optarg, sizeof(ami_host));
			break;
		case 'p':
			strncpy(ami_password, optarg, sizeof(ami_password));
			break;
		case 'r':
			always_refresh = 1;
			break;
		case 'u':
			strncpy(ami_username, optarg, sizeof(ami_username));
			break;
		default:
			fprintf(stderr, "Invalid option: %c\n", c);
			return -1;
		}
	}

	if (ami_username[0] && !ami_password[0] && !strcmp(ami_host, "127.0.0.1")) {
		/* If we're running as a privileged user with access to manager.conf, grab the password ourselves, which is more
		 * secure than getting as a command line arg from the user (and kind of convenient)
		 * Not that running as a user with access to the Asterisk config is great either, but, hey...
		 */
		if (ami_auto_detect_ami_pass(ami_username, ami_password, sizeof(ami_password))) {
			fprintf(stderr, "No password specified, and failed to autodetect from /etc/asterisk/manager.conf\n");
			return -1;
		}
	}

	if (!ami_username[0]) {
		fprintf(stderr, "No username provided (use -u flag)\n");
		return -1;
	}

	if (ami_connect(ami_host, 0, ami_callback, simple_disconnect_callback)) {
		return -1;
	}
	if (ami_action_login(ami_username, ami_password)) {
		fprintf(stderr, "Failed to log in with username %s\n", ami_username);
		return -1;
	}

	if (ttyspy()) {
		return -1;
	}
	return 0;
}
