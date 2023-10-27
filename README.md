# AsTTYSpy
Virtual TDD/TTY for Asterisk

## Background

This program is a simple utility that allows you to turn any terminal into a TTY/TDD. You can invoke the utility on any active Asterisk channel. The channel on which to invoke AsTTYSpy should be the channel to which you want to bridge, i.e. NOT the channel on your side of the conversation, but the channel on the other side. This utility will send the text you type onto that channel as Baudot code, as well as relay any Baudotcode received on the target channel to text that will appear in realtime on your terminal.

Because this utility can be used with any arbitrary channel, you can use it to arbitrarily receive TTY data from any channel and send TTY data to any channel, hence the name **AsTTYSpy**.

## Compiling

This program needs to be dynamically linked with CAMI.

The latest source for CAMI can be downloaded from: https://github.com/InterLinked1/cami

After you have built and installed CAMI, to compile, simply run "make".

Program Dependencies:
- CAMI:    https://github.com/InterLinked1/cami
- app_tdd: https://github.com/dgorski/app_tdd
- Asterisk `manager.conf` configuration:
-- Must have an AMI user with sufficient read/write permissions (call read/write).
-- The TddRxMsg event is essential to proper operation of this program, as well as being able to
   send TddTx actions.
-- The Newchannel and Hangup events are not strictly required, but auto refreshing of available channel selections
   will not work without these events. The DeviceStateChange may also be used instead. Alternately, if these
   events are not available for the AMI user used, you can specify -r to force a refresh every second.

This program is written for Linux distributions (tested on Debian).

## Getting Started

### Part A: Installing pre-requisites

You need an Asterisk system with the `app_tdd` module.

If you installed Asterisk using [PhreakScript](https://github.com/InterLinked1/phreakscript), you already have this.

If not, you can either reinstall Asterisk using PhreakScript or manually add `app_tdd.c` to your `apps` directory and reinstall Asterisk.

Secondly, the Asterisk Manager Interface (AMI) is also required. You will need to configure a user in `/etc/asterisk/manager.conf` with sufficient read and write permissions (read/write for `call` should be sufficient). It is recommended to create a dedicated AMI user for `AsTTYSpy`, with only the minimum privileges and events required.

### Part B: Building AsTTYSpy

1. Clone the CAMI repo: `https://github.com/InterLinked1/cami.git` and build it using the provided instructions
3. Download `asttyspy.c` and `Makefile` from this repo:
- `wget https://raw.githubusercontent.com/InterLinked1/AsTTYSpy/master/asttyspy.c`
- `wget https://raw.githubusercontent.com/InterLinked1/AsTTYSpy/master/Makefile`
4. Compile: `make`
5. Run program: `./asttyspy -h`

The program help will explain what options are available.

## Background and Usage

`AsTTYSpy` is mainly a testing program, intended for attaching a virtual console-based TTY to channels at will. In fact, it was developed mainly to test certain functionality, such as `app_tdd`, and used as a springboard for more complex programs. It is not intended as a full CA program, although the logic of this utility could be used to build such a program. However, this doesn't mean it isn't useful in and of itself. For example, if you don't have a TTY and you hear TTY tones on a phone call (running through your Asterisk system), you could use this to decode the Baudot code onto your terminal window in realtime. You can also use it to send Baudot code onto a channel. To put it simply, you can emulate having a TTY/TDD, to the extent you could use this for a 711 call.

Due to the nature of this program's development, it is currently limited on features, but requests and PRs are always welcome. Please report any bugs or issues through the GitHub issue tracker.
