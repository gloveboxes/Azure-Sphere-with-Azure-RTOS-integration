#include "applibs_versions.h"
#include "eventloop_timer_utilities.h"
#include "exit_codes.h"
#include "hw/azure_sphere_learning_path.h"
#include "stdbool.h"
#include <applibs/application.h>
#include <applibs/eventloop.h>
#include <applibs/gpio.h>
#include <applibs/log.h>
#include <applibs/powermanagement.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>


#define NELEMS(x)  (sizeof(x) / sizeof((x)[0]))
#define RT_APP_COMPONENT_LENGTH 36 + 1  // GUID 36 Char + 1 NULL terminate)

typedef enum {
	DIRECTION_UNKNOWN,
	INPUT,
	OUTPUT
} Direction;

struct _peripheral {
	int fd;
	int pin;
	GPIO_Value initialState;
	bool invertPin;
	bool (*initialise)(struct _peripheral* peripheral);
	char* name;
	Direction direction;
	bool opened;
};

typedef struct {
	void (*handler)(EventLoopTimer* timer);
	struct timespec period;
	EventLoopTimer* eventLoopTimer;
	const char* name;
} Timer;


typedef struct _peripheral Peripheral;
Peripheral** _peripheralSet = NULL;
size_t _peripheralSetCount = 0;

// GPIO support functions
bool OpenPeripheral(Peripheral* peripheral);
void OpenPeripheralSet(Peripheral** peripheralSet, size_t peripheralSetCount);
void ClosePeripheralSet(void);
void ClosePeripheral(Peripheral* peripheral);
void Gpio_On(Peripheral* peripheral);
void Gpio_Off(Peripheral* peripheral);

// Timer support functions
void StartTimerSet(Timer* timerSet[], size_t timerCount);
void StopTimerSet(void);
bool StartTimer(Timer* timer);
void StopTimer(Timer* timer);
EventLoop* GetTimerEventLoop(void);
void StopTimerEventLoop(void);
bool ChangeTimer(Timer* timer, const struct timespec* period);
bool SetOneShotTimer(Timer* timer, const struct timespec* delay);

Timer** _timers = NULL;
size_t _timerCount = 0;
EventLoop* eventLoop = NULL;

// inter core support functions
void SocketEventHandler(EventLoop* el, int fd, EventLoop_IoEvents events, void* context);
bool ProcessMsg(void);
void (*_interCoreCallback)(char*);
int sockFd = -1;

static EventRegistration* socketEventReg = NULL;

bool SendInterCoreMessage(const char* msg);
int EnableInterCoreCommunications(const char* rtAppComponentId, void (*interCoreCallback)(char*));

// Termination support functions
volatile sig_atomic_t terminationRequired = false;
volatile sig_atomic_t _exitCode = 0;

void RegisterTerminationHandler(void);
void TerminationHandler(int signalNumber);
void Terminate(int exitCode);
bool IsTerminationRequired(void);
int GetTerminationExitCode(void);


char rtAppComponentId[RT_APP_COMPONENT_LENGTH];  //initialized from cmdline argument

//extern volatile sig_atomic_t terminationRequired;
extern bool realTelemetry;		// flag for real or fake telemetry
void ProcessCmdArgs(int argc, char* argv[]);
char* GetCurrentUtc(char* buffer, size_t bufferSize);


char rtAppComponentId[RT_APP_COMPONENT_LENGTH];  //initialized from cmdline argument
//volatile sig_atomic_t terminationRequired = false;
bool realTelemetry = false;		// Generate fake telemetry or use Seeed Studio Grove SHT31 Sensor

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



// GPIO support functions

bool OpenPeripheral(Peripheral* peripheral) {
	if (peripheral == NULL || peripheral->pin < 0) { return false; }

	if (peripheral->opened) { return true; }

	if (peripheral->invertPin) {
		if (peripheral->initialState == GPIO_Value_High) {
			peripheral->initialState = GPIO_Value_Low;
		}
		else {
			peripheral->initialState = GPIO_Value_High;
		}
	}

	switch (peripheral->direction) {
	case OUTPUT:
		peripheral->fd = GPIO_OpenAsOutput(peripheral->pin, GPIO_OutputMode_PushPull, peripheral->initialState);
		if (peripheral->fd < 0) {
			Log_Debug(
				"Error opening GPIO: %s (%d). Check that app_manifest.json includes the GPIO used.\n",
				strerror(errno), errno);
			return false;
		}
		break;
	case INPUT:
		peripheral->fd = GPIO_OpenAsInput(peripheral->pin);
		if (peripheral->fd < 0) {
			Log_Debug(
				"Error opening GPIO: %s (%d). Check that app_manifest.json includes the GPIO used.\n",
				strerror(errno), errno);
			return false;
		}
		break;
	case DIRECTION_UNKNOWN:
		Log_Debug("Unknown direction for peripheral %s", peripheral->name);
		return false;
		break;
	}

	peripheral->opened = true;
	return true;
}

void OpenPeripheralSet(Peripheral** peripheralSet, size_t peripheralSetCount) {
	_peripheralSet = peripheralSet;
	_peripheralSetCount = peripheralSetCount;

	for (int i = 0; i < _peripheralSetCount; i++) {
		_peripheralSet[i]->fd = -1;
		if (_peripheralSet[i]->initialise != NULL) {
			if (!_peripheralSet[i]->initialise(_peripheralSet[i])) {
				Terminate(ExitCode_Open_Peripheral);
				break;
			}
		}
	}
}

/// <summary>
///     Closes a file descriptor and prints an error on failure.
/// </summary>
/// <param name="fd">File descriptor to close</param>
/// <param name="fdName">File descriptor name to use in error message</param>
void ClosePeripheral(Peripheral* peripheral) {
	if (peripheral->opened && peripheral->fd >= 0) {
		int result = close(peripheral->fd);
		if (result != 0) {
			Log_Debug("ERROR: Could not close peripheral %s: %s (%d).\n", peripheral->name == NULL ? "No name" : peripheral->name, strerror(errno), errno);
		}
	}
	peripheral->fd = -1;
	peripheral->opened = false;
}

void ClosePeripheralSet(void) {
	for (int i = 0; i < _peripheralSetCount; i++) {
		ClosePeripheral(_peripheralSet[i]);
	}
}

void Gpio_On(Peripheral* peripheral) {
	if (peripheral == NULL || peripheral->fd < 0 || peripheral->pin < 0 || !peripheral->opened) { return; }

	GPIO_SetValue(peripheral->fd, peripheral->invertPin ? GPIO_Value_Low : GPIO_Value_High);
}

void Gpio_Off(Peripheral* peripheral) {
	if (peripheral == NULL || peripheral->fd < 0 || peripheral->pin < 0 || !peripheral->opened) { return; }

	GPIO_SetValue(peripheral->fd, peripheral->invertPin ? GPIO_Value_High : GPIO_Value_Low);
}


// Timer functions

EventLoop* GetTimerEventLoop(void) {
	if (eventLoop == NULL) {
		eventLoop = EventLoop_Create();
	}
	return eventLoop;
}

bool ChangeTimer(Timer* timer, const struct timespec* period) {
	if (timer->eventLoopTimer == NULL) { return false; }
	timer->period.tv_nsec = period->tv_nsec;
	timer->period.tv_sec = period->tv_sec;
	int result = SetEventLoopTimerPeriod(timer->eventLoopTimer, period);

	return result == 0 ? true : false;
}

bool StartTimer(Timer* timer) {
	EventLoop* eventLoop = GetTimerEventLoop();
	if (eventLoop == NULL) {
		return false;
	}

	if (timer->eventLoopTimer != NULL) {
		return true;
	}

	if (timer->period.tv_nsec == 0 && timer->period.tv_sec == 0) {  // Set up a disabled Timer for oneshot or change timer
		timer->eventLoopTimer = CreateEventLoopDisarmedTimer(eventLoop, timer->handler);
		if (timer->eventLoopTimer == NULL) {
			return false;
		}
	}
	else {
		timer->eventLoopTimer = CreateEventLoopPeriodicTimer(eventLoop, timer->handler, &timer->period);
		if (timer->eventLoopTimer == NULL) {
			return false;
		}
	}


	return true;
}

void StopTimer(Timer* timer) {
	if (timer->eventLoopTimer != NULL) {
		DisposeEventLoopTimer(timer->eventLoopTimer);
		timer->eventLoopTimer = NULL;
	}
}

void StartTimerSet(Timer* timerSet[], size_t timerCount) {
	_timers = timerSet;
	_timerCount = timerCount;

	for (int i = 0; i < _timerCount; i++) {
		if (!StartTimer(_timers[i])) {
			break;
		};
	}
}

void StopTimerSet(void) {
	for (int i = 0; i < _timerCount; i++) {
		StopTimer(_timers[i]);
	}
}

void StopTimerEventLoop(void) {
	EventLoop* eventLoop = GetTimerEventLoop();
	if (eventLoop != NULL) {
		EventLoop_Close(eventLoop);
	}
}

bool SetOneShotTimer(Timer* timer, const struct timespec* period) {
	EventLoop* eventLoop = GetTimerEventLoop();
	if (eventLoop == NULL) {
		return false;
	}

	if (timer->eventLoopTimer == NULL) {
		return false;
	}

	if (SetEventLoopTimerOneShot(timer->eventLoopTimer, period) != 0) {
		return false;
	}

	return true;
}


// Inter core handlers

bool SendInterCoreMessage(const char* msg) {

	if (sockFd == -1) {
		Log_Debug("Socket not initialized");
		return false;
	}

	int bytesSent = send(sockFd, msg, strlen(msg), 0);
	if (bytesSent == -1) {
		Log_Debug("ERROR: Unable to send message: %d (%s)\n", errno, strerror(errno));
		return false;
	}

	return true;
}

int EnableInterCoreCommunications(const char* rtAppComponentId, void (*interCoreCallback)(char*)) {
	_interCoreCallback = interCoreCallback;
	// Open connection to real-time capable application.
	sockFd = Application_Connect(rtAppComponentId);
	if (sockFd == -1) {
		Log_Debug("ERROR: Unable to create socket: %d (%s)\n", errno, strerror(errno));
		return -1;
	}

	// Set timeout, to handle case where real-time capable application does not respond.
	static const struct timeval recvTimeout = { .tv_sec = 5, .tv_usec = 0 };
	int result = setsockopt(sockFd, SOL_SOCKET, SO_RCVTIMEO, &recvTimeout, sizeof(recvTimeout));
	if (result == -1) {
		Log_Debug("ERROR: Unable to set socket timeout: %d (%s)\n", errno, strerror(errno));
		return -1;
	}

	// Register handler for incoming messages from real-time capable application.
	socketEventReg = EventLoop_RegisterIo(GetTimerEventLoop(), sockFd, EventLoop_Input, SocketEventHandler,
		/* context */ NULL);
	if (socketEventReg == NULL) {
		Log_Debug("ERROR: Unable to register socket event: %d (%s)\n", errno, strerror(errno));
		return -1;
	}

	return 0;
}


/// <summary>
///     Handle socket event by reading incoming data from real-time capable application.
/// </summary>
void SocketEventHandler(EventLoop* el, int fd, EventLoop_IoEvents events, void* context) {
	if (!ProcessMsg()) {
		Terminate(ExitCode_InterCoreHandler);
	}
}


//Inter core functions

/// <summary>
///     Handle socket event by reading incoming data from real-time capable application.
/// </summary>
bool ProcessMsg() {
	char rxBuf[32];
	char msg[32];
	memset(msg, 0, sizeof msg);

	int bytesReceived = recv(sockFd, rxBuf, sizeof(rxBuf), 0);

	if (bytesReceived == -1) {
		//Log_Debug("ERROR: Unable to receive message: %d (%s)\n", errno, strerror(errno));
		return false;
	}

	//Log_Debug("Received %d bytes: ", bytesReceived);
	for (int i = 0; i < bytesReceived; ++i) {
		msg[i] = isprint(rxBuf[i]) ? rxBuf[i] : '.';
	}
	Log_Debug("\n%s\n\n", msg);

	_interCoreCallback(msg);

	return true;
}


// Termination support functions

void RegisterTerminationHandler(void) {
	struct sigaction action;
	memset(&action, 0, sizeof(struct sigaction));
	action.sa_handler = TerminationHandler;
	sigaction(SIGTERM, &action, NULL);
}

void TerminationHandler(int signalNumber) {
	// Don't use Log_Debug here, as it is not guaranteed to be async-signal-safe.
	terminationRequired = true;
	_exitCode = ExitCode_TermHandler_SigTerm;
}

void Terminate(int exitCode) {
	_exitCode = exitCode;
	terminationRequired = true;
}

bool IsTerminationRequired(void) {
	return terminationRequired;
}

int GetTerminationExitCode(void) {
	return _exitCode;
}


// Misc


void ProcessCmdArgs(int argc, char* argv[]) {
	do {
		argc--;
		switch (argc) {
		case 1:
			strncpy(rtAppComponentId, argv[1], RT_APP_COMPONENT_LENGTH);
			break;
		}
	} while (argc > 1);
}

char* GetCurrentUtc(char* buffer, size_t bufferSize) {
	time_t now = time(NULL);
	struct tm* t = gmtime(&now);
	strftime(buffer, bufferSize - 1, "%Y-%d-%m %H:%M:%S", t);
	return buffer;
}