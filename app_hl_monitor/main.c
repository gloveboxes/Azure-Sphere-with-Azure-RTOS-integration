/*********************************************************************************************************************

Disclaimer: Please read!

The learning_path_libs C Functions provided in the learning_path_libs folder:

	1) are NOT supported Azure Sphere APIs.
	2) are prefixed with lp_ (typedefs are prefixed with LP_)
	3) are built from the Azure Sphere SDK Samples provided at "https://github.com/Azure/azure-sphere-samples".
	4) are not intended as a substitute for understanding the Azure Sphere SDK Samples.
	5) aim to follow best practices as demonstrated by the Azure Sphere SDK Samples.
	6) are provided as is and as a convenience to aid the Azure Sphere Developer Learning experience.


Support developer boards for the Azure Sphere Developer Learning Path:

	1) The AVNET Azure Sphere Starter Kit.
	2) Seeed Studio Azure Sphere MT3620 Development Kit (aka Reference Design Board or rdb).
	3) Seeed Studio Seeed Studio MT3620 Mini Dev Board.

How to select your developer board

	1) Open CMakeLists.txt.
	2) Uncomment the set command that matches your developer board.
	3) File -> Save (ctrl+s) to save the file and Generate the CMake Cache.

**********************************************************************************************************************/

#include "hw/azure_sphere_learning_path.h"

#include "learning_path_libs/azure_iot.h"
#include "learning_path_libs/exit_codes.h"
#include "learning_path_libs/globals.h"
#include "learning_path_libs/inter_core.h"
#include "learning_path_libs/peripheral_gpio.h"
#include "learning_path_libs/terminate.h"
#include "learning_path_libs/timer.h"


#include "applibs_versions.h"
#include <applibs/gpio.h>
#include <applibs/log.h>
#include <applibs/powermanagement.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>


// Hardware specific

#ifdef OEM_AVNET
#include "learning_path_libs/AVNET/board.h"
#include "learning_path_libs/AVNET/imu_temp_pressure.h"
#include "learning_path_libs/AVNET/light_sensor.h"
#endif // OEM_AVNET

#ifdef OEM_SEEED_STUDIO
#include "learning_path_libs/SEEED_STUDIO/board.h"
#endif // SEEED_STUDIO

// Forward signatures
static void ButtonPressCheckHandler(EventLoopTimer* eventLoopTimer);
static void LedOn(LP_PeripheralGpio* led);
static void LedOffHandler(EventLoopTimer* eventLoopTimer);
static bool IsButtonPressed(LP_PeripheralGpio button, GPIO_Value_Type* oldState);

static const struct timespec ledStatusPeriod = { 2, 500 * 1000 * 1000 };

// GPIO Output Peripherals
static LP_PeripheralGpio ledRed = { .pin = LED_RED, .direction = LP_OUTPUT, .initialState = GPIO_Value_Low, .invertPin = true, .initialise = lp_openPeripheralGpio, .name = "ledRed" };
static LP_PeripheralGpio ledGreen = { .pin = LED_GREEN, .direction = LP_OUTPUT, .initialState = GPIO_Value_Low, .invertPin = true, .initialise = lp_openPeripheralGpio, .name = "ledGreen" };
static LP_PeripheralGpio ledBlue = { .pin = LED_BLUE, .direction = LP_OUTPUT, .initialState = GPIO_Value_Low, .invertPin = true, .initialise = lp_openPeripheralGpio, .name = "ledBlue" };
static LP_PeripheralGpio buttonA = { .pin = BUTTON_A, .direction = LP_INPUT, .initialise = lp_openPeripheralGpio, .name = "buttonA" };

// Timers
static LP_Timer ledOffOneShotTimer = { .period = { 0, 0 }, .name = "ledOffOneShotTimer", .handler = LedOffHandler };
static LP_Timer buttonPressCheckTimer = { .period = { 0, 1000000 }, .name = "buttonPressCheckTimer", .handler = ButtonPressCheckHandler };

// Initialize Sets
LP_PeripheralGpio* peripheralGpioSet[] = { &ledRed, &ledGreen, &ledBlue, &buttonA };
LP_Timer* timerSet[] = { &ledOffOneShotTimer, &buttonPressCheckTimer };


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
		lp_terminate(ExitCode_ButtonPressCheckHandler);
		return;
	}

	if (IsButtonPressed(buttonA, &buttonAState)) {

		lp_gpioOff(&ledRed);
		lp_gpioOff(&ledGreen);
		lp_gpioOff(&ledBlue);

		lp_sendInterCoreMessage("GetTemperature");
	}
}


/// <summary>
/// Turn on LED and set a one shot timer to turn LED2 off
/// </summary>
static void LedOn(LP_PeripheralGpio* led) {
	lp_gpioOn(led);
	lp_setOneShotTimer(&ledOffOneShotTimer, &ledStatusPeriod);
}


/// <summary>
/// One shot timer to turn LEDs off
/// </summary>
static void LedOffHandler(EventLoopTimer* eventLoopTimer) {
	if (ConsumeEventLoopTimerEvent(eventLoopTimer) != 0) {
		lp_terminate(ExitCode_Led2OffHandler);
		return;
	}

	lp_gpioOff(&ledRed);
	lp_gpioOff(&ledGreen);
	lp_gpioOff(&ledBlue);
}


/// <summary>
/// Read Button Peripheral returns pressed state
/// </summary>
static bool IsButtonPressed(LP_PeripheralGpio button, GPIO_Value_Type* oldState) {
	bool isButtonPressed = false;
	GPIO_Value_Type newState;

	if (GPIO_GetValue(button.fd, &newState) != 0) {
		lp_terminate(ExitCode_IsButtonPressed);
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
	lp_openPeripheralGpioSet(peripheralGpioSet, NELEMS(peripheralGpioSet));
	lp_startTimerSet(timerSet, NELEMS(timerSet));
	lp_enableInterCoreCommunications(rtAppComponentId, InterCoreMessageHandler);
}

/// <summary>
/// Close peripherals and handlers.
/// </summary>
static void ClosePeripheralsAndHandlers(void) {
	lp_stopTimerSet();
	lp_closePeripheralGpioSet();
	lp_stopTimerEventLoop();
}

int main(int argc, char* argv[]) {
	lp_registerTerminationHandler();
	lp_processCmdArgs(argc, argv);

	InitPeripheralsAndHandlers();

	// Main loop
	while (!lp_isTerminationRequired()) {
		int result = EventLoop_Run(lp_getTimerEventLoop(), -1, true);
		// Continue if interrupted by signal, e.g. due to breakpoint being set.
		if (result == -1 && errno != EINTR) {
			lp_terminate(ExitCode_Main_EventLoopFail);
		}
	}

	ClosePeripheralsAndHandlers();

	Log_Debug("Application exiting.\n");
	return lp_getTerminationExitCode();
}