#include <stdio.h>
#include <system.h>
#include <stdlib.h>
#include <string.h>
#include <altera_avalon_pio_regs.h>
#include <math.h>
#include "alt_types.h"
#include "altera_up_avalon_ps2.h"
#include "altera_up_ps2_keyboard.h"
#include "altera_up_avalon_video_character_buffer_with_dma.h"
#include "altera_up_avalon_video_pixel_buffer_dma.h"
#include "sys/alt_irq.h"
#include "freertosheader.h"

//Task priorities
#define	FREQUENCYCALCULATOR_PRIORITY	8
#define	STABILITYANALYSER_PRIORITY		7
#define	LOADMANAGER_PRIORITY			6
#define TIMERRESET_PRIORITY				5
#define DISPLAYLED_PRIORITY				4
#define WALLSWITCH_PRIORITY				3
#define	THRESHOLDMANAGER_PRIORITY		2
#define	VGADISPLAY_PRIORITY				1

//Queues
QueueHandle_t ADCQ;
QueueHandle_t FreqDataQ;
QueueHandle_t TimerQ;
QueueHandle_t ShedQ;
QueueHandle_t WallSwitchQ;
QueueHandle_t LEDQ;
QueueHandle_t KeyboardQ;
QueueHandle_t VGAQ;

//Macros
#define TASK_STACKSIZE    	2048
#define STD_QUEUE_SIZE  	30 	//standard queue size
#define BUFFER_SIZE 		10

//VGA macros
#define DATA_SIZE		25				//Data Size to be plotted
#define TEXT_Y	40						//pixel y position for character buffer
#define TEXT_X 	1						//pixel x position for character buffer
#define FREQ_X_ORIGIN	51				//pixel position at x axis for frequency graph
#define ROC_X_ORIGIN	371				//pixel position at x axis for RoC graph
#define X_STEP			8/(DATA_SIZE/25)//Number of pixels between data points
#define FREQ_Y_ORIGIN	175				//pixel position at y axis for frequency graph
#define FREQ_STEP		37.5			//Number of pixels per Hz
#define ROC_Y_ORIGIN	249				//pixel position at y axis for RoC graph
#define ROC_STEP		5				//Number of pixels per Hz/s
#define	MID_FREQ		50				//Middle frequency value on y axis

//Timers
TimerHandle_t timer500;

//Semaphores
SemaphoreHandle_t maintenanceSem;

//Structures
struct Sample {		//holds frequency data
	double sampleRoC;
	double sampleFreq;
	TickType_t time;
};
struct LED {		//holds LED data
	unsigned int red;
	unsigned int green;
};
struct ADC {		//holds the new ADC value and its time of arrival
	unsigned int adc;
	TickType_t adcTime;
};
struct Line {		//holds the values for line between two data points
	unsigned int x1;
	unsigned int y1;
	unsigned int x2;
	unsigned int y2;
};

//Global variables
double fThresh = 50;				// Frequency threshold (Hz)
double rocThresh = 20; 				// Rate of change threshold (Hz/s)
unsigned int stableBoolean = 0;	// Stability boolean for the most recent ADC values
unsigned int modeStatus = 0;// Status of the system (0: stable, 1: unstable, 2: maintenance)
unsigned int maintenanceFlag;// Flag for system to go to maintenance mode when system gets stable
//First load shedding time variables
TickType_t maxShedTime;
TickType_t startShedTime = 0;
TickType_t minShedTime = 1000;
TickType_t shedTimes[5];
TickType_t shedTime = 0;
double avgShedTime = 0;

char buffer[10];					//Holds the keyboard input values

void FrequencyISR() {
	struct ADC newADC;
	newADC.adc = IORD(FREQUENCY_ANALYSER_BASE, 0); //Read the ADC values
	newADC.adcTime = xTaskGetTickCountFromISR(); //get the time calculating first load shed times
	xQueueSendToBackFromISR(ADCQ, (void * ) &newADC, 0); //Send the ADC data over to FrequencyCalculator
}

//Calculates the frequency and RoC values
void FrequencyCalculator() {
	struct ADC newADC;
	struct Sample sample;
	double prevADC = 0;
	double prevFreq = 0;

	while (1) {
		xQueueReceive(ADCQ, &(newADC), portMAX_DELAY); //recieve the ADC data

		//Frequency calculation
		sample.sampleFreq = 16000 / (double) newADC.adc; //calculates and stores the signal frequency

		if (prevFreq == 0)	//first data sample
			sample.sampleRoC = 0;//rate of change is 0 for the first data sample
		else
			sample.sampleRoC = ((sample.sampleFreq - prevFreq) * 16000)
					/ ((double) (prevADC + newADC.adc) / 2);//calculates the rate of change of the frequency

		//keep track of ADC and frequency values to calculate rate of change
		prevADC = newADC.adc;
		prevFreq = sample.sampleFreq;

		sample.time = newADC.adcTime;

		xQueueSendToBack(VGAQ, (void * ) &sample, 0);//Sends the frequency data to VGADisplay task
		xQueueSendToBack(FreqDataQ, (void * ) &sample, 0); //Sends the frequency data to StabilityAnalyser task
	}
}

//Checks for any violations of the stability of the system
void StabilityAnalyser() {
	struct Sample sample;
	while (1) {
		xQueueReceive(FreqDataQ, &(sample), portMAX_DELAY);	//Receive the latest frequency data
		if (sample.sampleFreq < fThresh) {//if frequency is less than the threshold
			stableBoolean = 1; //Unstable
		} else if (fabs(sample.sampleRoC) > rocThresh) { //if RoC is less than the threshold
			stableBoolean = 1; // unstable
		} else {
			stableBoolean = 0; // stable
		}

		if ((stableBoolean == 1) && (modeStatus == 0)) { //if stability conditions are violated start shedding
			startShedTime = sample.time;
			xQueueSend(ShedQ, &stableBoolean, 0); //send shed signal to LoadManager task
		}
		xQueueSend(TimerQ, &stableBoolean, 0); //send stability signal to TimerReset task
	}
}

//Resets or starts the timer according to the status of the system and the stability boolean
void TimerReset() {
	unsigned int prevMode = modeStatus;
	unsigned int unstable;
	while (1) {
		xQueueReceive(TimerQ, &unstable, portMAX_DELAY); //Receive the stability signal
		if (xTimerIsTimerActive(timer500) == pdFALSE) { //Timer off
			if (modeStatus == 1) { //System unstable, start 500ms timer.
				xTimerReset(timer500, 0);
				xTimerStart(timer500, 0);
			}
		} else { //Timer on
			if ((unstable == 0) && ((modeStatus == 0) || (modeStatus == 2))) { //System and frequency are stable, timer stopped
				xTimerStop(timer500, 0);
			} else if ((prevMode != unstable) && (modeStatus == 1)) { //stability changed while system is unstable
				xTimerReset(timer500, 0);
				xTimerStart(timer500, 0);
			}
		}
		prevMode = unstable; //update previous status with new status
	}
}

//Callback function for the 500ms timer
void TimerCB() {
	unsigned int Shed = 0;
	if (stableBoolean == 1) { //System unstable for 500 ms
		Shed = 1;
	} else { //system stable for 500 ms
		Shed = 0;
	}
	xQueueSend(ShedQ, &Shed, 0); //Send the shed/reconnect signal to LoadManager
}

void MaintenanceISR(void* context, alt_u32 id) {
	int PressedButton = 0;
	int temp = IORD_ALTERA_AVALON_PIO_EDGE_CAP(PUSH_BUTTON_BASE);
	PressedButton = IORD_ALTERA_AVALON_PIO_DATA(PUSH_BUTTON_BASE);
	if (temp && PressedButton == 6) { //Key 1 pressed
		xSemaphoreTakeFromISR(maintenanceSem, pdFALSE);
		if (modeStatus == 0) { //when system stable change to maintenance immediately
			modeStatus = 2;
		} else if (modeStatus == 2) { //when in maintenance change to stable immediately
			modeStatus = 0;
		} else { //When unstable, raise the maintenance flag
			maintenanceFlag = 1;
		}
		xSemaphoreGiveFromISR(maintenanceSem, pdFALSE);
	}
	//Clear capture registers
	IOWR_ALTERA_AVALON_PIO_EDGE_CAP(PUSH_BUTTON_BASE, 0x7);
}

void LoadManager() {
	unsigned int RedLED = 0; //holds the red LED values
	unsigned int GreenLED = 0;	//holds the green LED values
	unsigned int SwitchValues = 0; //holds the switches values
	unsigned int shed = 0; //stores the shed signal
	unsigned int loads[5] = { 0 }; //stores the loads that are on
	unsigned int shedLoads[5] = { 0 }; //stores the loads that are shed
	unsigned int prevShedTime = 0, sumShedTime, numSheds = 0; //local variables for calculating first shed times
	int i = 0;

	while (1) {

		//Shed/Reconnect logic
		if (uxQueueMessagesWaiting(ShedQ) != 0) { //Check if there is a shed message in the queue
			xQueueReceive(ShedQ, &shed, 0);	//Recieve the shed signal
			if (RedLED != 0 || GreenLED != 0) {	// Checks if any of the loads are on/shed
				switch (shed) {
				case (0):
					for (i = 4; i >= 0; i--) { //Checks the highest priority load to reconnect
						if (shedLoads[i] == 1 && (SwitchValues & (1 << (i)))) { //Check if the load was shed and the switch is on
							//reconnect the load
							loads[i] = 1;
							shedLoads[i] = 0;
							break;
						}
					}
					break;
				case (1):
					for (i = 0; i < 5; i++) {//Checks the lowest priority load to shed
						if (loads[i] == 1) {	//if load is on
							//shed the load
							loads[i] = 0;
							shedLoads[i] = 1;
							break;
						}
					}
					break;
				}
			}
		}

		//Get switch values from WallSwitchPoll & sets LED values
		if (uxQueueMessagesWaiting(WallSwitchQ) != 0) {
			xQueueReceive(WallSwitchQ, &SwitchValues, 0);//receive the switch values
			for (int i = 0; i < 5; i++) {
				//Turn loads on/off if system is stable or in maintenance
				if (modeStatus == 0 || modeStatus == 2) {
					if (SwitchValues & (1 << i))
						loads[i] = 1;
					else
						loads[i] = 0;
				}
				//Only turn loads off when system is unstable
				if (!(SwitchValues & (1 << i))) {
					loads[i] = 0;
					shedLoads[i] = 0;
				}
			}
		}

		//reset LED values
		RedLED = 0;
		GreenLED = 0;

		struct LED LEDValues;

		//Calculate LED values from the array of loads
		for (int i = 0; i < 5; i++) {
			RedLED += (loads[i] * pow(2, i));	//binary to decimal conversion
		}
		for (int i = 0; i < 5; i++) {
			GreenLED += (shedLoads[i] * pow(2, i));
		}

		//Red LED 16 turns on while the maintenance flag is on
		if (maintenanceFlag == 1)
			RedLED += (1 << 16);
		//Red LED 17 turns on when in maintenance mode
		if (modeStatus == 2) {
			RedLED += (1 << 17);
		}

		//Save the values in a single struct to send over the queue
		LEDValues.green = GreenLED;
		LEDValues.red = RedLED;

		//Set the mode of the system
		xSemaphoreTake(maintenanceSem, portMAX_DELAY); //Need semaphore for critical section as modeStatus can be changed by ISR too
		//critical section
		for (i = 0; i < 5; i++) {
			if ((shedLoads[i] == 1) && (modeStatus != 2)) { //a load is shed & not maintenance mode
				modeStatus = 1; //mode is unstable
				break;
			} else if (i == 4) { //All loads have been reconnected
				if (maintenanceFlag == 1) { //Change to maintenance mode if flag was set
					maintenanceFlag = 0;
					modeStatus = 2;
				} else if (modeStatus != 2) { //Change to stable when system is not in maintenance
					modeStatus = 0;
				}
			}
		}
		//critical section end
		xSemaphoreGive(maintenanceSem);

		xQueueSend(LEDQ, (void * ) &LEDValues, 0); // Send LED Values over to DisplayLed task

		//Calculations for first load shed times
		if ((prevShedTime != startShedTime) && (modeStatus == 1)) {	//Checks if first load has been shed
			shedTime = xTaskGetTickCount() - startShedTime;	//Calculate time elapsed since deviation of F or RoC
			numSheds++;		//Number of first load sheds

			//Minimum time calculation
			if (shedTime < minShedTime)
				minShedTime = shedTime;
			//Maximum time calculation
			if (shedTime > maxShedTime)
				maxShedTime = shedTime;
			//Average time calculation
			sumShedTime += shedTime;
			avgShedTime = (double) sumShedTime / numSheds;

			//Manage overflow
			if (numSheds > 5) {
				shedTimes[0] = shedTimes[1];
				shedTimes[1] = shedTimes[2];
				shedTimes[2] = shedTimes[3];
				shedTimes[3] = shedTimes[4];
				shedTimes[4] = shedTime;
			} else
				shedTimes[numSheds - 1] = shedTime;
		}
		prevShedTime = startShedTime;

		vTaskDelay(1);	//Runs periodically every 1ms
	}

}

void WallSwitch() {
	unsigned int SwitchValue = 0;
	while (1) {
		SwitchValue = IORD_ALTERA_AVALON_PIO_DATA(SLIDE_SWITCH_BASE); //Get the switches' values
		SwitchValue &= (0x1F); //Keep only the values for the first five switches
		xQueueSendToBack(WallSwitchQ, &SwitchValue, 0);	//Send the values over to LoadManager
		vTaskDelay(50);	//Runs periodically every 50ms
	}
}
TickType_t time;

void DisplayLed() {
	//Initial setup -- Clear LEDs & allocate space for LEDValues
	IOWR_ALTERA_AVALON_PIO_DATA(GREEN_LEDS_BASE, 0);
	IOWR_ALTERA_AVALON_PIO_DATA(RED_LEDS_BASE, 0);
	struct LED LEDValues;	//Stores the LED values
	while (1) {
		xQueueReceive(LEDQ, &(LEDValues), portMAX_DELAY);//Receive the LED values

		//Set the red and green LED values
		IOWR_ALTERA_AVALON_PIO_DATA(RED_LEDS_BASE, (LEDValues.red));
		IOWR_ALTERA_AVALON_PIO_DATA(GREEN_LEDS_BASE, LEDValues.green);
	}
}

void KeyboardISR(void * context, alt_u32 id) {
	char ascii;
	int status = 0;
	unsigned char key = 0;
	KB_CODE_TYPE decode_mode;
	status = decode_scancode(context, &decode_mode, &key, &ascii);

	//Stop ISR, if not in maintenance mode
	if (modeStatus != 2) {
		return;
	}

	if (status == 0) {	//Input successfully read
		switch (decode_mode) {
		case KB_ASCII_MAKE_CODE:
			xQueueSendToBackFromISR(KeyboardQ, &ascii, 0);//Sends the ASCII value to ThresholdManager task
			break;
		case KB_BINARY_MAKE_CODE:
			xQueueSendToBackFromISR(KeyboardQ, &key, 0);//Sends the key value to ThresholdManager task
			break;
		default:
			break;
		}
	}
}

void ThresholdManager() {
	char input;
	int index = 0;
	int dotFlag = 0;
	while (1) {
		xQueueReceive(KeyboardQ, &input, portMAX_DELAY);//Receive the input from the KeyboardISR
		if (index < BUFFER_SIZE) {	//Only 10 characters are allowed
			if (input >= '0' && input <= '9') {
				buffer[index] = input;
				index++;
			}
			if (input == '.') {
				if (dotFlag == 0) {	//Checks if a decimal is already input before
					buffer[index] = input;
					index++;
				}
				dotFlag = 1;	//Raises the flag as only one decimal allowed
			}
		}
		if (input == 0x66) { //Backspace key pressed
			if (index > 0)	//Only goes back to the first index in the buffer
				index--;
			if (buffer[index] == '.')//Remove the dotFlag if decimal is backspaced
				dotFlag = 0;
			buffer[index] = 0;	//Set character to null
		}
		if (input == 'F') {	//Sets the buffer as the new frequency threshold
			fThresh = strtod(buffer, NULL);	//Convert char array to double and set new threshold
			//Clear buffer and other values
			memset(buffer, 0, 10);
			index = 0;
			dotFlag = 0;
		}
		if (input == 'R') {			//Sets the buffer as the new RoC threshold
			rocThresh = strtod(buffer, NULL);
			memset(buffer, 0, 10);
			index = 0;
			dotFlag = 0;
		}
	}
}

void VGADisplay() {
	//Initialise VGA controllers
	alt_up_pixel_buffer_dma_dev *pixel_buf;
	pixel_buf = alt_up_pixel_buffer_dma_open_dev(VIDEO_PIXEL_BUFFER_DMA_NAME);
	if (pixel_buf == NULL) {
		printf("Can't find pixel buffer device\n");
	}
	alt_up_pixel_buffer_dma_clear_screen(pixel_buf, 0);

	alt_up_char_buffer_dev *char_buf;
	char_buf = alt_up_char_buffer_open_dev(
			"/dev/video_character_buffer_with_dma");
	if (char_buf == NULL) {
		printf("Can't find char buffer device\n");
	}
	alt_up_char_buffer_clear(char_buf);

	struct Line freqLine, rocLine;
	//Creates the axes for graphs
	alt_up_pixel_buffer_dma_draw_hline(pixel_buf, 50, 250, 250,
			((0x3ff << 20) + (0x3ff << 10) + (0x3ff)), 0);
	alt_up_pixel_buffer_dma_draw_vline(pixel_buf, 50, 250, 100,
			((0x3ff << 20) + (0x3ff << 10) + (0x3ff)), 0);
	alt_up_pixel_buffer_dma_draw_hline(pixel_buf, 370, 570, 250,
			((0x3ff << 20) + (0x3ff << 10) + (0x3ff)), 0);
	alt_up_pixel_buffer_dma_draw_vline(pixel_buf, 370, 250, 100,
			((0x3ff << 20) + (0x3ff << 10) + (0x3ff)), 0);

	//Label the axes
	alt_up_char_buffer_string(char_buf, "Frequency(Hz)", 1, 10);
	alt_up_char_buffer_string(char_buf, "52", 3, 14);
	alt_up_char_buffer_string(char_buf, "51", 3, 18);
	alt_up_char_buffer_string(char_buf, "50", 3, 22);
	alt_up_char_buffer_string(char_buf, "49", 3, 26);
	alt_up_char_buffer_string(char_buf, "48", 3, 30);

	alt_up_char_buffer_string(char_buf, "RoC(Hz/s)", 41, 10);
	alt_up_char_buffer_string(char_buf, "30", 43, 15);
	alt_up_char_buffer_string(char_buf, "20", 43, 20);
	alt_up_char_buffer_string(char_buf, "10", 43, 25);
	alt_up_char_buffer_string(char_buf, "0", 43, 30);

	struct Sample sample;	//Stores the Frequency data
	char str[80];	//Used for character buffer
	TickType_t systemTime = 0;	//Stores the system's runtime

	//Data values for plotting the graph
	double fData[DATA_SIZE] = { 0 };
	double rocData[DATA_SIZE] = { 0 };
	int i = 0;

	while (1) {
		systemTime = xTaskGetTickCount() / 1000;	//Get the system's runtime

		//Display the threshold values
		sprintf(str, "Frequency threshold = %.3f Hz       RoC Threshold = %.3f",
				fThresh, rocThresh);
		alt_up_char_buffer_string(char_buf, str, TEXT_X, TEXT_Y);

		//Display system status
		switch (modeStatus) {
		case (0):
			alt_up_char_buffer_string(char_buf,
					"Status of the system: Stable     ", TEXT_X, TEXT_Y + 2);
			break;
		case (1):
			alt_up_char_buffer_string(char_buf,
					"Status of the system: Unstable   ", TEXT_X, TEXT_Y + 2);
			break;
		case (2):
			alt_up_char_buffer_string(char_buf,
					"Status of the system: Maintenance", TEXT_X, TEXT_Y + 2);
			break;
		}

		//Display Keyboard input in Maintanence mode
		if (modeStatus == 2) {
			alt_up_char_buffer_string(char_buf,
					"Enter new value:                ", TEXT_X, TEXT_Y + 4);

			sprintf(str, "Enter new value: %s", buffer);
			alt_up_char_buffer_string(char_buf, str, TEXT_X, TEXT_Y + 4);
			sprintf(str,
					"Press F to set as frequency threshold or R to set as RoC threshold");
			alt_up_char_buffer_string(char_buf, str, TEXT_X, TEXT_Y + 5);
		}
		//Otherwise delete lines
		else {
			alt_up_char_buffer_string(char_buf,
					"                               ", TEXT_X, TEXT_Y + 4);
			alt_up_char_buffer_string(char_buf,
					"                                                                  ",
					TEXT_X, TEXT_Y + 5);
		}
		//Display system run time
		sprintf(str, "System run time: %lus", systemTime);
		alt_up_char_buffer_string(char_buf, str, TEXT_X, TEXT_Y + 7);

		//Display five recent load shed times
		sprintf(str,
				"Recent first load shed times(ms): %lu, %lu, %lu, %lu, %lu",
				shedTimes[0], shedTimes[1], shedTimes[2], shedTimes[3],
				shedTimes[4]);
		alt_up_char_buffer_string(char_buf, str, TEXT_X, TEXT_Y + 9);

		//Display first load shed time statistics
		sprintf(str,
				"Max shed time= %lu ms   Min shed time= %lu ms, Avg shed time= %.3f ms        ",
				maxShedTime, minShedTime, avgShedTime);
		alt_up_char_buffer_string(char_buf, str, TEXT_X, TEXT_Y + 11);

		while (uxQueueMessagesWaiting(VGAQ) != 0) {	//Save as much data from the queue
			xQueueReceive(VGAQ, &(sample), portMAX_DELAY);
			fData[i] = sample.sampleFreq;
			rocData[i] = fabs(sample.sampleRoC);
			i = (i + 1) % DATA_SIZE;	//Cycle again after overflow
		}

		//Empty the graph area
		alt_up_pixel_buffer_dma_draw_box(pixel_buf, 51, 101, 251, 249, 0, 0);
		alt_up_pixel_buffer_dma_draw_box(pixel_buf, 371, 101, 571, 249, 0, 0);

		if (fData[DATA_SIZE - 1] != 0) {//Start when all values have been recieved
			for (int j = 0; j < DATA_SIZE; j++) {
				freqLine.x1 = FREQ_X_ORIGIN + (j * X_STEP);	//First frequency x value
				freqLine.x2 = FREQ_X_ORIGIN + ((j + 1) * X_STEP);//Next frequency x value
				freqLine.y1 = (int) (FREQ_Y_ORIGIN
						+ FREQ_STEP * (MID_FREQ - fData[(i + j) % DATA_SIZE]));	//First frequency y value //starts with i(oldest value) to newest(i + datasize))
				freqLine.y2 = (int) (FREQ_Y_ORIGIN
						+ FREQ_STEP
								* (MID_FREQ - fData[(i + j + 1) % DATA_SIZE]));	//Next frequency y value

				rocLine.x1 = ROC_X_ORIGIN + (j * X_STEP);	//First RoC x value
				rocLine.x2 = ROC_X_ORIGIN + ((j + 1) * X_STEP);	//Next RoC x value
				rocLine.y1 = (int) (ROC_Y_ORIGIN
						- ROC_STEP * (rocData[(i + j) % DATA_SIZE]));//First RoC y value
				rocLine.y2 = (int) (ROC_Y_ORIGIN
						- ROC_STEP * (rocData[(i + j + 1) % DATA_SIZE]));//Next RoC x value

				//Draw the lines between the data points
				alt_up_pixel_buffer_dma_draw_line(pixel_buf, freqLine.x1,
						freqLine.y1, freqLine.x2, freqLine.y2, 0x3ff << 10, 0);
				alt_up_pixel_buffer_dma_draw_line(pixel_buf, rocLine.x1,
						rocLine.y1, rocLine.x2, rocLine.y2, 0x3ff << 10, 0);
			}
		}
		vTaskDelay(10);	//Runs periodically every 20ms
	}
}

//Setup the ISRS
void initISRSetup() {
	//Setup keyboard ISR
	alt_up_ps2_dev * ps2_device = alt_up_ps2_open_dev(PS2_NAME);
	alt_up_ps2_clear_fifo(ps2_device);
	alt_irq_register(PS2_IRQ, ps2_device, KeyboardISR);
	IOWR_8DIRECT(PS2_BASE, 4, 1);
	if (ps2_device == NULL) {
		printf("can't find PS/2 device\n");
	}
	alt_irq_register(FREQUENCY_ANALYSER_IRQ, 0, FrequencyISR);
	IOWR_ALTERA_AVALON_PIO_EDGE_CAP(PUSH_BUTTON_BASE, 0x7);
	IOWR_ALTERA_AVALON_PIO_IRQ_MASK(PUSH_BUTTON_BASE, 0x1); //Only enable interrupt on first key
	alt_irq_register(PUSH_BUTTON_IRQ, NULL, MaintenanceISR);
	return;
}

//Setup the queues, timers and semaphores
void initOSDataStructs() {
	ADCQ = xQueueCreate(STD_QUEUE_SIZE, sizeof(struct ADC));
	ShedQ = xQueueCreate(STD_QUEUE_SIZE, sizeof(void*));
	WallSwitchQ = xQueueCreate(STD_QUEUE_SIZE, sizeof(unsigned int));
	LEDQ = xQueueCreate(STD_QUEUE_SIZE, sizeof(struct LED));
	FreqDataQ = xQueueCreate(STD_QUEUE_SIZE, sizeof(struct Sample));
	TimerQ = xQueueCreate(STD_QUEUE_SIZE, sizeof(unsigned int));
	KeyboardQ = xQueueCreate(STD_QUEUE_SIZE, sizeof(void*));
	VGAQ = xQueueCreate(STD_QUEUE_SIZE, sizeof(struct Sample));
	timer500 = xTimerCreate("timerM", 500, pdTRUE, NULL, TimerCB);
	maintenanceSem = xSemaphoreCreateCounting(9999, 1);
	return;
}

//Setup the tasks
void initCreateTasks() {
	xTaskCreate(DisplayLed, "DisplayLED", TASK_STACKSIZE, NULL,
			DISPLAYLED_PRIORITY, NULL);
	xTaskCreate(WallSwitch, "WallSwitch", TASK_STACKSIZE, NULL,
			WALLSWITCH_PRIORITY, NULL);
	xTaskCreate(FrequencyCalculator, "FrequencyCalculator", TASK_STACKSIZE,
			NULL, FREQUENCYCALCULATOR_PRIORITY, NULL);
	xTaskCreate(StabilityAnalyser, "StabilityAnalyser", TASK_STACKSIZE, NULL,
			STABILITYANALYSER_PRIORITY, NULL);
	xTaskCreate(TimerReset, "TimerReset", TASK_STACKSIZE, NULL,
			TIMERRESET_PRIORITY, NULL);
	xTaskCreate(LoadManager, "LoadManager", TASK_STACKSIZE, NULL,
			LOADMANAGER_PRIORITY, NULL);
	xTaskCreate(ThresholdManager, "ThresholdManager", TASK_STACKSIZE, NULL,
			THRESHOLDMANAGER_PRIORITY, NULL);
	xTaskCreate(VGADisplay, "VGADisplay", TASK_STACKSIZE, NULL,
			VGADISPLAY_PRIORITY, NULL);
	return;
}

//Entry
int main() {
	initISRSetup();
	initOSDataStructs();
	initCreateTasks();
	vTaskStartScheduler();
	for (;;)
		;

	return 0;
}
