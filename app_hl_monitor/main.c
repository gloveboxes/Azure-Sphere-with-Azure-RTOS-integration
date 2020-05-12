#include "../Hardware/avnet_mt3620_sk/inc/hw/azure_sphere_learning_path.h"
#include "applibs_versions.h"
#include "libs/globals.h"
#include "libs/inter_core.h"
#include "libs/peripheral.h"
#include "libs/terminate.h"
#include "libs/timer.h"
#include <applibs/gpio.h>
#include <applibs/log.h>
#include <applibs/powermanagement.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

// Forward signatures
static void ButtonPressCheckHandler(EventLoopTimer* eventLoopTimer);
static void LedOn(Peripheral* led);
static void LedOffHandler(EventLoopTimer* eventLoopTimer);
static bool IsButtonPressed(Peripheral button, GPIO_Value_Type* oldState);

static const struct timespec ledStatusPeriod = { 2, 500 * 1000 * 1000 };

// GPIO Output Peripherals
static Peripheral ledRed = { .pin = LED_RED, .direction = OUTPUT, .initialState = GPIO_Value_Low, .invertPin = true, .initialise = OpenPeripheral, .name = "ledRed" };
static Peripheral ledGreen = { .pin = LED_GREEN, .direction = OUTPUT, .initialState = GPIO_Value_Low, .invertPin = true, .initialise = OpenPeripheral, .name = "ledGreen" };
static Peripheral ledBlue = { .pin = LED_BLUE, .direction = OUTPUT, .initialState = GPIO_Value_Low, .invertPin = true, .initialise = OpenPeripheral, .name = "ledBlue" };
static Peripheral buttonA = { .pin = BUTTON_A, .direction = INPUT, .initialise = OpenPeripheral, .name = "buttonA" };

// Timers
static Timer ledOffOneShotTimer = { .period = { 0, 0 }, .name = "ledOffOneShotTimer", .handler = LedOffHandler };
static Timer buttonPressCheckTimer = { .period = { 0, 1000000 }, .name = "buttonPressCheckTimer", .handler = ButtonPressCheckHandler };

// Initialize Sets
Peripheral* peripheralSet[] = { &ledRed, &ledGreen, &ledBlue, &buttonA };
Timer* timerSet[] = { &ledOffOneShotTimer, &buttonPressCheckTimer };


/// <summary>
/// Callback handler for Inter-Core Messaging 
/// </summary>
static void InterCoreMessageHandler(char* msg) {
	static float previousTemperature = 0.0;

	float temperature = strtof(msg, NULL);

	if (temperature == previousTemperature) {
		LedOn(&ledGreen);
	}

	if (temperature < previousTemperature) {
		LedOn(&ledBlue);
	}

	if (temperature > previousTemperature) {
		LedOn(&ledRed);
	}

	previousTemperature = temperature;

	Log_Debug(msg);
}


/// <summary>
/// Handler to check for Button Presses
/// </summary>
static void ButtonPressCheckHandler(EventLoopTimer* eventLoopTimer) {
	static GPIO_Value_Type buttonAState;

	if (ConsumeEventLoopTimerEvent(eventLoopTimer) != 0) {
		Terminate(ExitCode_ButtonPressCheckHandler);
		return;
	}

	if (IsButtonPressed(buttonA, &buttonAState)) {

		Gpio_Off(&ledRed);
		Gpio_Off(&ledGreen);
		Gpio_Off(&ledBlue);

		SendInterCoreMessage("GetTemperature");
	}
}


/// <summary>
/// Turn on LED and set a one shot timer to turn LED2 off
/// </summary>
static void LedOn(Peripheral* led) {
	Gpio_On(led);
	SetOneShotTimer(&ledOffOneShotTimer, &ledStatusPeriod);
}


/// <summary>
/// One shot timer to turn LEDs off
/// </summary>
static void LedOffHandler(EventLoopTimer* eventLoopTimer) {
	if (ConsumeEventLoopTimerEvent(eventLoopTimer) != 0) {
		Terminate(ExitCode_Led2OffHandler);
		return;
	}

	Gpio_Off(&ledRed);
	Gpio_Off(&ledGreen);
	Gpio_Off(&ledBlue);
}


/// <summary>
/// Read Button Peripheral returns pressed state
/// </summary>
static bool IsButtonPressed(Peripheral button, GPIO_Value_Type* oldState) {
	bool isButtonPressed = false;
	GPIO_Value_Type newState;

	if (GPIO_GetValue(button.fd, &newState) != 0) {
		Terminate(ExitCode_IsButtonPressed);
	}
	else {
		// Button is pressed if it is low and different than last known state.
		isButtonPressed = (newState != *oldState) && (newState == GPIO_Value_Low);
		*oldState = newState;
	}
	return isButtonPressed;
}


/// <summary>
///  Initialize peripherals, device twins, direct methods, timers.
/// </summary>
static void InitPeripheralsAndHandlers(void) {
	OpenPeripheralSet(peripheralSet, NELEMS(peripheralSet));
	StartTimerSet(timerSet, NELEMS(timerSet));
	EnableInterCoreCommunications(rtAppComponentId, InterCoreMessageHandler);
}

/// <summary>
/// Close peripherals and handlers.
/// </summary>
static void ClosePeripheralsAndHandlers(void) {
	StopTimerSet();
	ClosePeripheralSet();
	StopTimerEventLoop();
}

int main(int argc, char* argv[]) {
	RegisterTerminationHandler();
	ProcessCmdArgs(argc, argv);

	InitPeripheralsAndHandlers();

	// Main loop
	while (!IsTerminationRequired()) {
		int result = EventLoop_Run(GetTimerEventLoop(), -1, true);
		// Continue if interrupted by signal, e.g. due to breakpoint being set.
		if (result == -1 && errno != EINTR) {
			Terminate(ExitCode_Main_EventLoopFail);
		}
	}

	ClosePeripheralsAndHandlers();

	Log_Debug("Application exiting.\n");
	return GetTerminationExitCode();
}