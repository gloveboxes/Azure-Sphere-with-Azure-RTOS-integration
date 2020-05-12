#include "../../Hardware/avnet_mt3620_sk/inc/hw/azure_sphere_learning_path.h"
#include "i2c.h"
#include "lsm6dso_driver.h"
#include "lsm6dso_reg.h"
#include "mt3620-intercore.h"
#include "os_hal_gpio.h"
#include "os_hal_uart.h"
#include "printf.h"
#include "tx_api.h"
#include <stdbool.h>


#define DEMO_STACK_SIZE         1024
#define DEMO_BYTE_POOL_SIZE     9120
#define DEMO_BLOCK_POOL_SIZE    100
#define DEMO_QUEUE_SIZE         100


// resources for inter core messaging
static uint8_t buf[256];
static uint32_t dataSize;
static BufferHeader* outbound, * inbound;
static uint32_t sharedBufSize = 0;
static const size_t payloadStart = 20;
bool highLevelReady = false;


// Define the ThreadX object control blocks...
TX_THREAD               tx_thread_inter_core;
TX_THREAD               tx_thread_read_button;
TX_THREAD               tx_thread_read_sensor;
TX_THREAD               tx_thread_blink_led;
TX_EVENT_FLAGS_GROUP    event_flags_0;
TX_BYTE_POOL            byte_pool_0;
TX_BLOCK_POOL           block_pool_0;
UCHAR                   memory_area[DEMO_BYTE_POOL_SIZE];


// Define thread prototypes.
void thread_inter_core(ULONG thread_input);
void thread_read_sensor(ULONG thread_input);
void thread_blink_led(ULONG thread_blink);
int gpio_output(u8 gpio_no, u8 level);


int main() {
	tx_kernel_enter();	// Enter the Azure RTOS kernel.
}


// Define what the initial system looks like.
void tx_application_define(void* first_unused_memory) {
	CHAR* pointer;
	
	tx_byte_pool_create(&byte_pool_0, "byte pool 0", memory_area, DEMO_BYTE_POOL_SIZE);		// Create a byte memory pool from which to allocate the thread stacks
	

	tx_byte_allocate(&byte_pool_0, (VOID**)&pointer, DEMO_STACK_SIZE, TX_NO_WAIT);			// Allocate the stack for inter core thread
	tx_thread_create(&tx_thread_inter_core, "thread inter core", thread_inter_core, 0,		// Create inter core message thread
		pointer, DEMO_STACK_SIZE, 1, 1, TX_NO_TIME_SLICE, TX_AUTO_START);
	

	tx_byte_allocate(&byte_pool_0, (VOID**)&pointer, DEMO_STACK_SIZE, TX_NO_WAIT);			// Allocate the stack for thread_blink_led thread
	tx_thread_create(&tx_thread_blink_led, "thread blink led", thread_blink_led, 0,			// Create button press thread */
		pointer, DEMO_STACK_SIZE, 4, 4, TX_NO_TIME_SLICE, TX_AUTO_START);
	
	
	tx_byte_allocate(&byte_pool_0, (VOID**)&pointer, DEMO_STACK_SIZE, TX_NO_WAIT);			// Allocate the stack for read sensor thread
	tx_thread_create(&tx_thread_read_sensor, "thread read sensor", thread_read_sensor, 0,	// Create read sensor thread */
		pointer, DEMO_STACK_SIZE, 4, 4, TX_NO_TIME_SLICE, TX_AUTO_START);

	
	tx_event_flags_create(&event_flags_0, "event flags 0");									// Create event flag for thread sync
}


void thread_inter_core(ULONG thread_input) {
	UINT status;
	
	if (GetIntercoreBuffers(&outbound, &inbound, &sharedBufSize) == -1) {					// Initialize Inter-Core Communications
		for (;;) { // empty.			
		}
	}

	// This thread monitors inter core messages.
	while (1) {
		dataSize = sizeof(buf);
		int r = DequeueData(outbound, inbound, sharedBufSize, buf, &dataSize);

		if (r == 0 && dataSize > payloadStart) {
			highLevelReady = true;

			// Set event flag 0 to wakeup threads read sensor and blink led
			status = tx_event_flags_set(&event_flags_0, 0x1, TX_OR);

			if (status != TX_SUCCESS)
				break;
		}

		tx_thread_sleep(25);
	}
}


void thread_blink_led(ULONG thread_blink) {
	UINT status;
	ULONG   actual_flags;

	while (true) {
		// waits here until flag set in inter core thread
		status = tx_event_flags_get(&event_flags_0, 0x1, TX_OR_CLEAR,
			&actual_flags, TX_WAIT_FOREVER);

		if ((status != TX_SUCCESS) || (actual_flags != 0x1))
			break;

		gpio_output(LED2, 0);
		tx_thread_sleep(50);
		gpio_output(LED2, 1);
	}
}


void thread_read_sensor(ULONG thread_input) {
	UINT    status;
	ULONG   actual_flags;
	float temperature;

	mtk_os_hal_i2c_ctrl_init(i2c_port_num);		// Initialize MT3620 I2C bus
	i2c_enum();									// Enumerate I2C Bus

	if (i2c_init()) {
		return;
	}

	if (lsm6dso_init(i2c_write, i2c_read)) {	// LSM6DSO Init - the accelerometer calibration has been commented out for faster start up 
		return;
	}

	while (true) {
		// waits here until flag set in inter core thread
		status = tx_event_flags_get(&event_flags_0, 0x1, TX_OR_CLEAR, &actual_flags, TX_WAIT_FOREVER);  // wait until event set in the thread_inter_core thread

		if ((status != TX_SUCCESS) || (actual_flags != 0x1))
			break;

		if (highLevelReady) {

			lsm6dso_show_result();
			temperature = get_temperature();

			snprintf((char*)buf + payloadStart, sizeof(buf) - payloadStart, "%.2f", temperature);
			dataSize = payloadStart + strlen((char*)buf + payloadStart); // payloadStart + telemetry length;

			EnqueueData(inbound, outbound, sharedBufSize, buf, dataSize);
		}
	}
}


int gpio_output(u8 gpio_no, u8 level) {
	int ret;

	ret = mtk_os_hal_gpio_request(gpio_no);
	if (ret != 0) {
		printf("request gpio[%d] fail\n", gpio_no);
		return ret;
	}
	mtk_os_hal_gpio_set_direction(gpio_no, OS_HAL_GPIO_DIR_OUTPUT);
	mtk_os_hal_gpio_set_output(gpio_no, level);
	ret = mtk_os_hal_gpio_free(gpio_no);
	if (ret != 0) {
		printf("free gpio[%d] fail\n", gpio_no);
		return ret;
	}
	return 0;
}