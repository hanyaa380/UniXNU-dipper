// SPDX-License-Identifier: MIT
// Register the board's own classes into the kernel kmod in OSKext::load's
// lock order. Calling preModLoad/postModLoad directly can deadlock against a
// kext loading on an IOKit config thread.
#pragma once
#include <libkern/OSReturn.h>
#include <libkern/OSKextLib.h>
extern "C" OSReturn OSKextRegisterBoardClasses(const char *kextIdentifier, void (*constructors)(void));
