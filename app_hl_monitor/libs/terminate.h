#pragma once

#include "globals.h"
#include "exit_codes.h"

void RegisterTerminationHandler(void);
void TerminationHandler(int signalNumber);
void Terminate(int exitCode);
bool IsTerminationRequired(void);
int GetTerminationExitCode(void);