/*
 * Copyright (C) 2021-2024 Roger Clark, VK3KYY / G4KYF
 *                         Colin Durbridge, G4EML
 *                         Daniel Caujolle-Bert, F1RMB
 *
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer
 *    in the documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * 4. Use of this source code or binary releases for commercial purposes is strictly forbidden. This includes, without limitation,
 *    incorporation in a commercial product or incorporation into a product or project which allows commercial use.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <stdbool.h>
#include <memory.h>
#include <malloc.h>
#include <stdio.h>

#include "hardware/ST7567.h"
#include "interfaces/adc.h"
#include "interfaces/batteryRAM.h"
#include "hardware/SPI_Flash.h"
#include "interfaces/adc.h"
#include "hardware/radioHardwareInterface.h"

#include "io/buttons.h"
#include "usb/usb_com.h"
#include "usb_device.h"
#include "user_interface/menuSystem.h"
#include "user_interface/uiUtilities.h"
#include "user_interface/uiLocalisation.h"
#include "functions/ticks.h"
#include "interfaces/batteryAndPowerManagement.h"
#include "interfaces/gps.h"
#include "interfaces/settingsStorage.h"
#include "interfaces/remoteHead.h"

#if defined(USING_EXTERNAL_DEBUGGER)
#include "SeggerRTT/RTT/SEGGER_RTT.h"
#endif

// Couldn't declare into aprs.h, due to header cross-dependence.
void aprsBeaconingTick(uiEvent_t *ev);

volatile bool mainIsRunning = true;
static bool updateMessageOnScreen = false;
char globalFailureMessage[SCREEN_LINE_BUFFER_SIZE] = { 0 };
bool spiFlashInitHasFailed = false;

#if ! defined(PLATFORM_GD77S)
ticksTimer_t autolockTimer;
#endif


#if (__NVIC_PRIO_BITS != 3)
#error Files need to be manually after regernation using the STM32Cube configuration tool to support the non-genuine CPU

/*
 * To support the Non-genuine CPU in newer radios, the following changes must be made
 *
 * stm32f405xx.h line 49 needs to be changed to
 * #define __NVIC_PRIO_BITS          3U       // VK3KYY changed this to 3 to fix problem with clone CPU. Genuine STM32F4XX uses 4 Bits for the Priority Levels
 *
 * FreeRTOS
 *
 * FreeRTOSConfig.h
 *
 * line 132
 *
 *  #define configPRIO_BITS         3
 *
 *  port.c
 *
 *  line 346
 *
 * 	configASSERT( ( portMAX_PRIGROUP_BITS - ulMaxPRIGROUPValue ) >= configPRIO_BITS );
 *
 * Also     HAL_NVIC_SetPriority(PendSV_IRQn, 15, 0);  has to be changed to HAL_NVIC_SetPriority(PendSV_IRQn, 7, 0); in stm32f4xx_hal_msp.h
 *
 */

#endif


#if false
static void hexDump(uint8_t *ptr, int len)
{
	char buf[8];
	char msg[128];

	msg[0] = 0;
	for (int i = 0; i < len; i++)
	{
		if ((i != 0) && (i%16 == 0))
		{
			strcat(msg,"\n");
			CDC_Transmit_FS((uint8_t *)msg,strlen(msg));
			msg[0] = 0;
		}
		else
		{
			sprintf(buf,"%02x ",*ptr);
			strcat(msg,buf);
		}
		ptr++;
		osDelay(1);
	}
}
#endif


static void keyBeepHandler(uiEvent_t *ev, bool ptttoggleddown)
{
	bool isScanning = (uiVFOModeIsScanning() || uiChannelModeIsScanning()) && !uiVFOModeSweepScanning(false);

	// Do not send any beep while scanning, otherwise enabling the AMP will be handled as a valid signal detection.
	if (((ev->keys.event & (KEY_MOD_LONG | KEY_MOD_PRESS)) == (KEY_MOD_LONG | KEY_MOD_PRESS)) ||
			((ev->keys.event & KEY_MOD_UP) == KEY_MOD_UP))
	{

		if ((ptttoggleddown == false) && (isScanning == false))
		{
			if (nonVolatileSettings.audioPromptMode >= AUDIO_PROMPT_MODE_BEEP)
			{
				soundSetMelody(nextKeyBeepMelody);
			}
			else
			{
				soundSetMelody(MELODY_KEY_BEEP);
			}
			nextKeyBeepMelody = (int16_t *)MELODY_KEY_BEEP;// set back to the default beep
		}
		else
		{ 	// Reset the beep sound if we are scanning, otherwise the AudioAssist
			// beep will be played instead of the normal one.

			if (isScanning)
			{
				if (melody_play != NULL)
				{
					soundStopMelody();
				}
			}
			else
			{
				soundSetMelody(MELODY_KEY_BEEP);
			}
		}
	}
	else
	{
		if ((ev->keys.event & (KEY_MOD_LONG | KEY_MOD_DOWN)) == (KEY_MOD_LONG | KEY_MOD_DOWN))
		{
			if ((ptttoggleddown == false) && (isScanning == false))
			{
				if (nextKeyBeepMelody != MELODY_KEY_BEEP)
				{
					soundSetMelody(nextKeyBeepMelody);
					nextKeyBeepMelody = (int16_t *)MELODY_KEY_BEEP;
				}
				else
				{
					soundSetMelody(MELODY_KEY_LONG_BEEP);
				}
			}
		}
	}
}

static bool rxBeepsMustPlayMelody(uint8_t rxBitOption)
{
	return ((nonVolatileSettings.beepOptions & rxBitOption) && (settingsUsbMode != USB_MODE_HOTSPOT) &&
			((uiDataGlobal.Scan.active == false) || (uiDataGlobal.Scan.active && (uiDataGlobal.Scan.state == SCAN_STATE_PAUSED))) &&
			(nonVolatileSettings.audioPromptMode != AUDIO_PROMPT_MODE_SILENT) && (voicePromptsIsPlaying() == false));
}

static void rxBeepsCarrierEndCallback(void)
{
	soundSetMelody(MELODY_RX_BEEP_END_BEEP);
}

static bool rxBeepsHandler(void)
{
	if (uiDataGlobal.rxBeepState & RX_BEEP_CARRIER_HAS_STARTED_EXEC)
	{
		// Don't clear the RF start beep when scanning, it has to be played, so wait the scan has paused.
		if ((uiDataGlobal.Scan.active &&
				((uiDataGlobal.Scan.state == SCAN_STATE_SCANNING) || (uiDataGlobal.Scan.state == SCAN_STATE_SHORT_PAUSED))) == false)
		{
			uiDataGlobal.rxBeepState &= ~RX_BEEP_CARRIER_HAS_STARTED_EXEC;
		}

		if (rxBeepsMustPlayMelody(BEEP_RX_CARRIER))
		{
			// Cancel queued rx end beep, if any. In this case, there is no
			// need to play another start beep, as the previous one has already
			// been heard.
			if (cancelTimerCallback(rxBeepsCarrierEndCallback, MENU_ANY) == false)
			{
				soundSetMelody(MELODY_RX_BEEP_BEGIN_BEEP);
			}

			return true;
		}
	}
	else if (uiDataGlobal.rxBeepState & RX_BEEP_TALKER_HAS_STARTED_EXEC)
	{
		uiDataGlobal.rxBeepState &= ~(RX_BEEP_TALKER_HAS_STARTED_EXEC | RX_BEEP_TALKER_HAS_ENDED | RX_BEEP_TALKER_HAS_ENDED_EXEC);

		if ((nonVolatileSettings.beepOptions & BEEP_RX_TALKER_BEGIN) && rxBeepsMustPlayMelody(BEEP_RX_TALKER))
		{
			soundSetMelody(MELODY_RX_BEEP_CALLER_BEGIN_BEEP);

			return true;
		}
	}
	else if (uiDataGlobal.rxBeepState & RX_BEEP_TALKER_HAS_ENDED_EXEC)
	{
		// FM: Replace Carrier beeps with Talker beeps if Caller beep option is selected.
		if ((trxGetMode() == RADIO_MODE_ANALOG) && ((nonVolatileSettings.beepOptions & BEEP_RX_CARRIER) == 0) && (nonVolatileSettings.beepOptions & BEEP_RX_TALKER))
		{
			uiDataGlobal.rxBeepState = RX_BEEP_UNSET;
		}
		else
		{
			uiDataGlobal.rxBeepState &= ~(RX_BEEP_TALKER_HAS_ENDED_EXEC | RX_BEEP_TALKER_HAS_STARTED | RX_BEEP_TALKER_HAS_STARTED_EXEC | RX_BEEP_TALKER_IDENTIFIED);
		}

		if (rxBeepsMustPlayMelody(BEEP_RX_TALKER))
		{
			soundSetMelody(MELODY_RX_BEEP_CALLER_END_BEEP);

			return true;
		}
	}
	else if (uiDataGlobal.rxBeepState & RX_BEEP_CARRIER_HAS_ENDED)
	{
		uiDataGlobal.rxBeepState = RX_BEEP_UNSET;

		if (rxBeepsMustPlayMelody(BEEP_RX_CARRIER))
		{
			// Delays end beep tone by 100ms, as it could played immediately after CALLER_END_BEEP on some conditions
			(void)addTimerCallback(rxBeepsCarrierEndCallback, 150, MENU_ANY, false);

			return true;
		}
	}

	return false;
}

static bool validateUpdateCallback(void)
{
	if (uiDataGlobal.MessageBox.keyPressed == KEY_GREEN)
	{
		updateMessageOnScreen = false;
		settingsSetOptionBit(BIT_SETTINGS_UPDATED, false);

		// G4EML added for remote front panel support because it currently cant power down the radio.
		if(remoteHeadActive)
		{
			settingsSaveSettings(false);
		}

		return true;
	}

	return false;
}

static void settingsUpdateAudioAlert(void)
{
	if (nonVolatileSettings.audioPromptMode >= AUDIO_PROMPT_MODE_VOICE_THRESHOLD)
	{
		voicePromptsInit();
		voicePromptsAppendPrompt(PROMPT_SETTINGS_UPDATE);
		voicePromptsPlay();
	}
	else
	{
		soundSetMelody(MELODY_ACK_BEEP);
	}
}

static void latchedSk2Callback(void)
{
	if(uiDataGlobal.sk2latched)
	{
		uiDataGlobal.sk2latched = false;
		soundSetMelody(MELODY_NSK2_BEEP);
	}
}

void applicationMainTask(void)
{
	keyboardCode_t keys;
	int key_event = EVENT_KEY_NONE;
	//  int keyFunction = 0;
	uint32_t buttons = 0;
	int button_event = EVENT_BUTTON_NONE;
	uint32_t rotary = 0;
	int rotary_event = EVENT_ROTARY_NONE;
	int function_event = 0;
	bool keyOrButtonChanged = false;
	int16_t *quickkeyPushedMenuMelody = NULL;
	int keyFunction;
	bool wasRestoringDefaultsettings = false;
	uiEvent_t ev = { .buttons = 0, .keys = NO_KEYCODE, .rotary = 0, .function = 0, .events = NO_EVENT, .hasEvent = false, .time = ticksGetMillis() };
	bool safeBootMode = false;
	bool forceSafeBootMode = false;

	remoteHeadInit();

	gpsPower(true);				//Turn on the power to the Microphone and GPS module
	HAL_GPIO_WritePin(SPI_Flash_CS_GPIO_Port, SPI_Flash_CS_Pin, GPIO_PIN_SET);// disable the Flash

#if defined(USING_EXTERNAL_DEBUGGER)
	SEGGER_RTT_ConfigUpBuffer(0, NULL, NULL, 0, SEGGER_RTT_MODE_NO_BLOCK_TRIM);
	SEGGER_RTT_printf(0,"Segger RTT initialised\n");
#endif

	buttonsFrontPanelInit();
	buttonsInit();
	keyboardInit();

	/*
	 HAL_TIM_Base_Start_IT(&htim6);
	 uint32_t count = 5;
	 aprsIsrFlag=false;
	 while (!aprsIsrFlag && count>0)
	 {
		 vTaskDelay((1000 / portTICK_PERIOD_MS));
		 count--;
	 }
	 */

	// Operator asked for settings reset.
	// With the modified frontpanel button Col/Row, it's not
	// possible to use anything else than P3 if the transceiver was
	// powered off using the power button.
	if(remoteHeadActive)
	{
		remoteHeadRestartIP();			//rescan and resend the key state
		vTaskDelay(10);			//allow time for this to complete.
	}

	wasRestoringDefaultsettings = settingsLoadSettings(((buttonsFrontPanelRead() & FRONT_KEY_P3) == FRONT_KEY_P3) && !((buttonsFrontPanelRead() & FRONT_KEY_DOWN) == FRONT_KEY_DOWN));

	radioPowerOn();

	displayInit((settingsIsOptionBitSet(BIT_AUTO_NIGHT_OVERRIDE) ?
			((uiDataGlobal.daytimeOverridden == NIGHT) ^ settingsIsOptionBitSet(BIT_INVERSE_VIDEO)) : settingsIsOptionBitSet(BIT_INVERSE_VIDEO)));
	displaySetContrast(nonVolatileSettings.displayContrast);

	uiDataGlobal.dmrDisabled = !codecIsAvailable();  // Check if DMR codec is available

	spiFlashInitHasFailed = !SPI_Flash_init();
	if (spiFlashInitHasFailed)
	{
		safeBootBranching:

		if (spiFlashInitHasFailed)
		{
			nonVolatileSettings.batteryCalibration = (0x05) + (0x07 << 4); // needed to get a correct default voltage reading
		}

		displayEnableBacklight(true, 100);
		
		// Since the settings weren't read from the Flash, set backlight percentage
		nonVolatileSettings.displayBacklightPercentage[DAY] = 100;
		displayEnableBacklight(true, -1);
 		gpioSetDisplayBacklightIntensityPercentage(100);
 
		menuDataGlobal.controlData.stack[0] = MENU_EMPTY;
		menuDataGlobal.currentItemIndex = 0;

		snprintf(globalFailureMessage, SCREEN_LINE_BUFFER_SIZE, "%s", (safeBootMode ? "SAFE BOOT" : "FLASH MEM ERROR"));
		showErrorMessage(globalFailureMessage);
		
		die(true, false, false, safeBootMode);
	}

	radioInit();

	calibrationInit();

	HRC6000Init();

	radioPostinit();

	HAL_ADC_Start_IT(&hadc1);

	HRC6000InitTask();

	voxInit();

	menuRadioInfosInit();
	batteryUpdate();

	soundInitBeepTask();

	// Clear boot melody and image
#if defined(PLATFORM_MD9600)
	if ((buttonsFrontPanelRead() & (FRONT_KEY_P3 | FRONT_KEY_DOWN)) == (FRONT_KEY_P3 | FRONT_KEY_DOWN))
#else
	if ((buttons & BUTTON_SK2) && (keyboardRead() == KEY_FRONT_DOWN))
#endif
	{
		settingsEraseCustomContent();
#if !defined(PLATFORM_MD9600)
		themeInit(true);
#endif
	}

	// Safe boot mode
#if defined(PLATFORM_MD9600)
	if (forceSafeBootMode || (((buttonsFrontPanelRead() & FRONT_KEY_P3) == FRONT_KEY_P3) && (micButtonsRead() == KEY_0)))
#else
	if (forceSafeBootMode || ((buttons & BUTTON_SK1) && (keyboardRead() == KEY_0)))
#endif
	{
#if !defined(PLATFORM_MD9600)
		themeInit(false);
#endif
		safeBootMode = true;

		goto safeBootBranching;
	}



	lastHeardInitList();
	codeplugInitCaches();
	dmrIDCacheInit();
	voicePromptsCacheInit();

	if (wasRestoringDefaultsettings || (micButtonsRead() == KEY_HASH))
	{
		enableVoicePromptsIfLoaded((micButtonsRead() == KEY_HASH));
	}

	// Need to take care if the user has already been fully notified about the settings update
	wasRestoringDefaultsettings = settingsIsOptionBitSet(BIT_SETTINGS_UPDATED);

	// Should be initialized before the splash screen, as we don't want melodies when VOX is enabled
	voxSetParameters(nonVolatileSettings.voxThreshold, nonVolatileSettings.voxTailUnits);

	codeplugGetSignallingDTMFDurations(&uiDataGlobal.DTMFContactList.durations);

#if !defined(PLATFORM_MD9600)
	if (settingsIsOptionBitSet(BIT_SAFE_POWER_ON) && ((buttons & BUTTON_SK1) != BUTTON_SK1))
	{
		powerOffFinalStage(true, true);
	}
#endif

	menuSystemInit(getRtcTime_custom());

#if defined(HAS_GPS)
	gpsInit();

#if defined(LOG_GPS_DATA)
	if ((micButtonsRead() == KEY_5) && ((buttonsFrontPanelRead() & FRONT_KEY_DOWN) == FRONT_KEY_DOWN))
	{
		uiNotificationShow(NOTIFICATION_TYPE_MESSAGE, NOTIFICATION_ID_MESSAGE, 1500, "NMEA Clearing", true);
		gpsLoggingClear();
		settingsSet(nonVolatileSettings.gpsLogMemOffset, 0U);
	}
#endif

	if (nonVolatileSettings.gps != GPS_MODE_OFF)
	{
		gpsOn();
#if defined(LOG_GPS_DATA)
		gpsLoggingStart();
#endif
	}

#endif

	ticksTimerStart(&apoTimer, ((nonVolatileSettings.apo * 30) * 60000U));
	ticksTimerStart(&autolockTimer, (nonVolatileSettings.autolockTimer * 30000U));

	aprsBeaconingInit();
	aprsBeaconingStart();

	/* Infinite loop */
	for(;;)
	{
		uint16_t frontPanelButtons = FRONT_KEY_NONE;
		uint32_t startTime = ticksGetMillis();
		bool syntheticEvent = false; // used to not trigger the backlight on faked key/button events

		mainIsRunning = true;
		keyOrButtonChanged = false;

		tick_com_request();
		handleTimerCallbacks();
		batteryUpdate();

		// Ignore any input when APRS is TXing (avoiding changing the channel in the middle of a packet).
		if ((currentChannelData->aprsConfigIndex == 0) ||
				((currentChannelData->aprsConfigIndex != 0) && (aprsTxProgress == APRS_TX_IDLE)))
		{
			// Hack so that keypad is not read at all once a CPS transfer is initiated, as reading the mic keypad seems to cause delays which
			// delay the CPS processing and cause and error to be sent to the CPS
			frontPanelButtons = buttonsFrontPanelRead();
			keyboardCheckKeyEvent(&keys, &key_event, frontPanelButtons); // Read keyboard state and event
			// frontPanelButtons variable will have ALARM, P1 and P2 states cleared, as they will
			// become ORANGE, SK2 and SK1 buttons, after calling buttonsCheckButtonsEvent()
			buttonsCheckButtonsEvent(&buttons, &button_event, (keys.key != 0), uiDataGlobal.sk2latched, &frontPanelButtons); // Read button state and event
		}
		else
		{
			if (aprsTxProgress == APRS_TX_FINISHED)
			{
				// Force the UI to handle the button release.
				button_event = EVENT_BUTTON_CHANGE;
				key_event = EVENT_KEY_CHANGE;
				keyboardReset();
				syntheticEvent = true;
			}
			else
			{
				button_event = EVENT_BUTTON_NONE;
				key_event = EVENT_KEY_NONE;
				keyboardReset();
			}
		}

		// Lockscreen shouldn't be called on KEY_POWER down event,
		// otherwise it won't be possible to turn the transceiver OFF without
		// unlocking the keypad first, pretty annoying.
		if (keypadLocked && KEYCHECK_DOWN(keys, KEY_POWER))
		{
			syntheticEvent = true;
		}

		// Handle UP and not PRESS to avoid ON->OFF cycles.
		// Hence, the operator has to release the button to turn the radio ON or OFF
		if (KEYCHECK_UP(keys, KEY_POWER))
		{
			powerDown((buttons & (BUTTON_ORANGE | BUTTON_SK2)) == (BUTTON_ORANGE | BUTTON_SK2));
		}

		if (((key_event != EVENT_KEY_NONE) && (keys.key != 0)) || (button_event != EVENT_BUTTON_NONE) || (rotary_event != EVENT_ROTARY_NONE))
		{
			keyOrButtonChanged = true;

			if((!trxIsTransmitting) & (menuSystemGetCurrentMenuNumber() != MENU_NUMERICAL_ENTRY))
			{
				if ((uiDataGlobal.sk2latched == false) && KEYCHECK_LONGDOWN(keys, KEY_D))
				{
					keys.key = 0;
					key_event = EVENT_KEY_NONE;
					uiDataGlobal.sk2latched = true;
					soundSetMelody(MELODY_SK2_BEEP);
				}

				if (uiDataGlobal.sk2latched && KEYCHECK_UP(keys, KEY_D)) //dont send a beep when D key released
				{
					keys.key = 0;
					key_event = EVENT_KEY_NONE;
				}

				if ((uiDataGlobal.sk2latched == false) && KEYCHECK_SHORTUP(keys, KEY_D))
				{
					buttons |= BUTTON_SK2_SHORT_UP;
					button_event = EVENT_BUTTON_CHANGE;
					// pretend this is not a key event
					keys.key = 0;
					keys.event = 0;
					key_event = EVENT_KEY_NONE;
					soundSetMelody(MELODY_KEY_BEEP);
				}
				else if (KEYCHECK_UP(keys, KEY_ORANGE))
				{
					buttons |= BUTTON_ORANGE_SHORT_UP;
					button_event = EVENT_BUTTON_CHANGE;
					// pretend this is not a key event
					keys.key = 0;
					keys.event = 0;
					key_event = EVENT_KEY_NONE;
				}
				else if (uiDataGlobal.sk2latched && KEYCHECK_SHORTUP(keys, KEY_A))
				{
					uiDataGlobal.sk2latched = false;
					keys.key = 0;
					keys.event = 0;
					key_event = EVENT_KEY_NONE;
					soundSetMelody(MELODY_NSK2_BEEP);
				}

				// Once the keypad is locked, clear any LONG/REPEAT events
				if (keypadLocked && ((keys.key == KEY_D) && (keys.event & (KEY_MOD_DOWN | KEY_MOD_LONG | KEY_MOD_PRESS))))
				{
					keys.key = 0;
					key_event = EVENT_KEY_NONE;
					button_event = EVENT_BUTTON_NONE;
					buttons &= ~BUTTON_SK2;
				}

				if (uiDataGlobal.sk2latched && (keys.key != KEY_D))
				{
					// if keypad is locked, only set SK2 modifier while pressing a key
					if ((keypadLocked == false) || (keypadLocked && (keys.key != 0)))
					{
						button_event = EVENT_BUTTON_CHANGE;
						buttons |= BUTTON_SK2;
					}

					(void)addTimerCallback(latchedSk2Callback, 3000, MENU_ANY, true);
				}
			}

		}

		// Just to trigger the backlight ATM
		if ((frontPanelButtons & FRONT_KEY_P3) == FRONT_KEY_P3)
		{
			button_event = EVENT_BUTTON_CHANGE;
		}

		// Settings reset information screen
		if (wasRestoringDefaultsettings && (menuSystemGetRootMenuNumber() != UI_SPLASH_SCREEN))
		{
			wasRestoringDefaultsettings = false;
			updateMessageOnScreen = true;

			menuSystemPushNewMenu(MENU_LANGUAGE);// As language menu is now in Options, present the operator with the Language menu after the "Settings updated" message has been displayed.

			snprintf(uiDataGlobal.MessageBox.message, MESSAGEBOX_MESSAGE_LEN_MAX, "%s", "Настройки\nсброшены");
			uiDataGlobal.MessageBox.type = MESSAGEBOX_TYPE_INFO;
			uiDataGlobal.MessageBox.decoration = MESSAGEBOX_DECORATION_FRAME;
			uiDataGlobal.MessageBox.buttons =
#if defined(PLATFORM_MD9600)
					MESSAGEBOX_BUTTONS_ENT;
#else
					MESSAGEBOX_BUTTONS_OK;
#endif
			uiDataGlobal.MessageBox.validatorCallback = validateUpdateCallback;
			menuSystemPushNewMenu(UI_MESSAGE_BOX);

			(void)addTimerCallback(settingsUpdateAudioAlert, 100, UI_MESSAGE_BOX, false);// Need to delay playing this for a while, because otherwise it may get played before the volume is turned up enough to hear it.
		}

		// VOX Checking
		if (voxIsEnabled())
		{
			// if a key/button event happen, reset the VOX.
			if ((key_event == EVENT_KEY_CHANGE) || (button_event == EVENT_BUTTON_CHANGE) || (keys.key != 0) || (buttons != BUTTON_NONE))
			{
				voxReset();
			}
			else
			{
				if (!trxTransmissionEnabled && voxIsTriggered() && ((buttons & BUTTON_PTT) == 0))
				{
					button_event = EVENT_BUTTON_CHANGE;
					buttons |= BUTTON_PTT;
				}
				else if (trxTransmissionEnabled && ((voxIsTriggered() == false) || (keys.event & KEY_MOD_PRESS)))
				{
					button_event = EVENT_BUTTON_CHANGE;
					buttons &= ~BUTTON_PTT;
				}
				else if (trxTransmissionEnabled && voxIsTriggered())
				{
					// Any key/button event reset the vox
					if ((button_event != EVENT_BUTTON_NONE) || (keys.event != EVENT_KEY_NONE))
					{
						voxReset();
						button_event = EVENT_BUTTON_CHANGE;
						buttons &= ~BUTTON_PTT;
					}
					else
					{
						buttons |= BUTTON_PTT;
					}
				}
			}
		}

		// If the settings update message is still on screen, don't permit to start xmitting.
		if (updateMessageOnScreen && (buttons & BUTTON_PTT))
		{
			button_event = EVENT_BUTTON_CHANGE;
			buttons &= ~BUTTON_PTT;
		}

		if (headerRowIsDirty == true)
		{
			int currentMenu = menuSystemGetCurrentMenuNumber();

			if ((currentMenu == UI_CHANNEL_MODE) || (currentMenu == UI_VFO_MODE) ||
					((currentMenu == MENU_SATELLITE) && menuSatelliteIsDisplayingHeader()))
			{
				bool sweeping;
				if ((sweeping = uiVFOModeSweepScanning(true)))
				{
					displayFillRect(0, 0, DISPLAY_SIZE_X, 9, true);
				}
				else
				{
#if defined(PLATFORM_RD5R)
					displayFillRect(0, 0, DISPLAY_SIZE_X, 9, true);
#else
					displayFillRect(0, 0, DISPLAY_SIZE_X, 14, true); // 2 rows are two much (16 pixels), switched to FillRect.
#endif
				}
				uiUtilityRenderHeader(uiVFOModeDualWatchIsScanning(), sweeping);
				displayRenderRows(0, 2);
			}

			headerRowIsDirty = false;
		}

		if (keypadLocked || PTTLocked)
		{
			if (keypadLocked && ((buttons & BUTTON_PTT) == 0))
			{
				if ((key_event == EVENT_KEY_CHANGE) && (syntheticEvent == false))
				{
					bool continueToFilterKeys = true;

					// A key is pressed, but a message box is currently displayed (probably a private call notification)
					if (menuSystemGetCurrentMenuNumber() == UI_MESSAGE_BOX)
					{
						// Clear any key but RED and GREEN
						if ((keys.key == KEY_RED) || (keys.key == KEY_GREEN))
						{
							continueToFilterKeys = false;
						}
					}

					if (continueToFilterKeys)
					{
						if ((PTTToggledDown == false) && (menuSystemGetCurrentMenuNumber() != UI_LOCK_SCREEN))
						{
							menuSystemPushNewMenu(UI_LOCK_SCREEN);
						}

						key_event = EVENT_KEY_NONE;
					}

					if (settingsIsOptionBitSet(BIT_PTT_LATCH) && PTTToggledDown)
					{
						PTTToggledDown = false;
					}
				}

				// Lockout ORANGE AND BLUE (BLACK stay active regardless lock status, useful to trigger backlight)
				if ((button_event == EVENT_BUTTON_CHANGE) && ((buttons & BUTTON_ORANGE) || (buttons & BUTTON_SK2)))
				{
					if ((PTTToggledDown == false) && (menuSystemGetCurrentMenuNumber() != UI_LOCK_SCREEN))
					{
						menuSystemPushNewMenu(UI_LOCK_SCREEN);
					}

					button_event = EVENT_BUTTON_NONE;

					if (settingsIsOptionBitSet(BIT_PTT_LATCH) && PTTToggledDown)
					{
						PTTToggledDown = false;
					}
				}
			}
			else if (PTTLocked)
			{
				if ((buttons & BUTTON_PTT) && (button_event == EVENT_BUTTON_CHANGE))
				{
					// PTT button is pressed, but a message box is currently displayed, and PC allowance is set to PTT,
					// hence it's probably a private call accept, so let the PTT button being handled later in the code
					if (((menuSystemGetCurrentMenuNumber() == UI_MESSAGE_BOX) && (nonVolatileSettings.privateCalls == ALLOW_PRIVATE_CALLS_PTT)) == false)
					{
						soundSetMelody(MELODY_ERROR_BEEP);

						if (menuSystemGetCurrentMenuNumber() != UI_LOCK_SCREEN)
						{
							menuSystemPushNewMenu(UI_LOCK_SCREEN);
						}

						button_event = EVENT_BUTTON_NONE;
						// Clear PTT button
						buttons &= ~BUTTON_PTT;
					}
				}
				else if ((buttons & BUTTON_SK2) && KEYCHECK_DOWN(keys, KEY_STAR))
				{
					if (menuSystemGetCurrentMenuNumber() != UI_LOCK_SCREEN)
					{
						menuSystemPushNewMenu(UI_LOCK_SCREEN);
					}
				}
			}
		}

		int trxMode = trxGetMode();

		// Long press RED
		if ((key_event == EVENT_KEY_CHANGE) && ((buttons & BUTTON_PTT) == 0) && (keys.key != 0))
		{
			int currentMenu = menuSystemGetCurrentMenuNumber();

			// Longpress RED send back to root menu, it's only available from
			// any menu but VFO, Channel, UI_MESSAGE_BOX and CPS
			if (((currentMenu != UI_CHANNEL_MODE) && (currentMenu != UI_VFO_MODE) && (currentMenu != UI_CPS) && (currentMenu != UI_MESSAGE_BOX)) &&
					KEYCHECK_LONGDOWN(keys, KEY_RED) && (uiVFOModeIsScanning() == false) && (uiChannelModeIsScanning() == false))
			{
				if (currentMenu != MENU_SATELLITE)
				{
					// If an option menu is currently running, ensure the
					// settings copy is reset.
					resetOriginalSettingsData();
				}

				uiDataGlobal.currentSelectedContactIndex = 0;
				menuSystemPopAllAndDisplayRootMenu();
				soundSetMelody(MELODY_KEY_BEEP);

				// Clear button/key event/state.
				buttons = BUTTON_NONE;
				rotary = 0;
				key_event = EVENT_KEY_NONE;
				button_event = EVENT_BUTTON_NONE;
				rotary_event = EVENT_ROTARY_NONE;
				keys.key = 0;
				keys.event = 0;
			}
		}
		//
		// PTT toggle feature
		//
		// PTT is locked down, but any button, except SK1 or SK2(1750Hz in FM) or DTMF Key in Analog,
		// or Up/Down with LH on screen in Digital, is pressed, virtually release PTT
		if ((settingsIsOptionBitSet(BIT_PTT_LATCH) && PTTToggledDown) &&
				(((button_event & EVENT_BUTTON_CHANGE) && (
#if ! defined(PLATFORM_RD5R)
						(buttons & BUTTON_ORANGE) ||
#endif
						((trxMode != RADIO_MODE_ANALOG) && (buttons & BUTTON_SK2)))) ||
						((keys.key != 0) && (keys.event & KEY_MOD_UP) &&
								(((trxMode == RADIO_MODE_ANALOG) && keyboardKeyIsDTMFKey(keys.key)) == false) &&
								(((trxMode == RADIO_MODE_DIGITAL) && menuTxScreenDisplaysLastHeard() && ((keys.key == KEY_UP) || (keys.key == KEY_DOWN))) == false))))
		{
			PTTToggledDown = false;
			button_event = EVENT_BUTTON_CHANGE;
			buttons = BUTTON_NONE;
			key_event = EVENT_KEY_NONE;
			keys.key = 0;
		}

		// PTT toggle action
		if (settingsIsOptionBitSet(BIT_PTT_LATCH))
		{
			if (button_event == EVENT_BUTTON_CHANGE)
			{
				if (buttons & BUTTON_PTT)
				{
					if (PTTToggledDown == false)
					{
						// PTT toggle works only if a TOT value is defined.
						if (currentChannelData->tot != 0)
						{
							PTTToggledDown = true;
						}
					}
					else
					{
						PTTToggledDown = false;
					}
				}
			}

			if (PTTToggledDown && ((buttons & BUTTON_PTT) == 0))
			{
				buttons |= BUTTON_PTT;
			}
		}
		else
		{
			if (PTTToggledDown)
			{
				PTTToggledDown = false;
			}
		}

		if(trxGetMode() == RADIO_MODE_ANALOG)
		{
			trxCheckAnalogSquelch();
		}
		else
		{
			if (ticksTimerHasExpired((ticksTimer_t *)&readDMRRSSITimer))
			{
				ticksTimerStart((ticksTimer_t *)&readDMRRSSITimer, 100); // hold off for a while (this will be changed by HRC6000.c if we are receiving a signal)
				trxReadRSSIAndNoise(true);
			}
		}

		if (!trxTransmissionEnabled && (updateLastHeard == true))
		{
			lastHeardListUpdate((uint8_t *)DMR_frame_buffer, false);
			updateLastHeard = false;
		}

		if ((nonVolatileSettings.hotspotType == HOTSPOT_TYPE_OFF) ||
				((nonVolatileSettings.hotspotType != HOTSPOT_TYPE_OFF) && (settingsUsbMode != USB_MODE_HOTSPOT))) // Do not filter anything in HS mode.
		{
			if ((uiDataGlobal.PrivateCall.state == PRIVATE_CALL_DECLINED) &&
					(slotState == DMR_STATE_IDLE))
			{
				menuPrivateCallClear();
			}

			if ((trxTransmissionEnabled == false) && (trxIsTransmitting == false) &&
					(uiDataGlobal.displayQSOState == QSO_DISPLAY_CALLER_DATA) && (nonVolatileSettings.privateCalls > ALLOW_PRIVATE_CALLS_OFF))
			{
				if (HRC6000GetReceivedTgOrPcId() == (trxDMRID | (PC_CALL_FLAG << 24)))
				{
					int receivedSrcId = HRC6000GetReceivedSrcId();

					if ((uiDataGlobal.PrivateCall.state == PRIVATE_CALL_NOT_IN_CALL) &&
							(trxTalkGroupOrPcId != (receivedSrcId | (PC_CALL_FLAG << 24))) &&
							(receivedSrcId != uiDataGlobal.PrivateCall.lastID))
					{
						if ((receivedSrcId & 0xFFFFFF) >= 1000000)
						{
							menuSystemPushNewMenu(UI_PRIVATE_CALL);
						}
					}
				}
			}
		}

		if (button_event == EVENT_BUTTON_CHANGE)
		{
			// Toggle backlight
			if (nonVolatileSettings.backlightMode == BACKLIGHT_MODE_MANUAL)
			{
				if (buttons == BUTTON_SK1) // SK1 alone
				{
					displayEnableBacklight(! displayIsBacklightLit(), nonVolatileSettings.displayBacklightPercentageOff);
				}
			}
			else
			{
				if (syntheticEvent == false)
				{
					displayLightTrigger(true);
				}
			}

			if ((buttons & BUTTON_PTT) != 0)
			{
				int currentMenu = menuSystemGetCurrentMenuNumber();

				if ((trxGetMode() != RADIO_MODE_NONE) &&
						(settingsUsbMode != USB_MODE_HOTSPOT) &&
						(currentMenu != UI_POWER_OFF) &&
						(currentMenu != UI_SPLASH_SCREEN) &&
						(currentMenu != UI_TX_SCREEN)&&
						(currentMenu != MENU_CALIBRATION))
				{
					bool wasScanning = false;

					if (uiDataGlobal.Scan.active || uiDataGlobal.Scan.toneActive)
					{
						if (currentMenu == UI_VFO_MODE)
						{
							uiVFOModeStopScanning();
						}
						else
						{
							uiChannelModeStopScanning();
						}
						wasScanning = true;
					}
					else
					{
						if (currentMenu == UI_LOCK_SCREEN)
						{
							menuLockScreenPop();
						}
					}

					currentMenu = menuSystemGetCurrentMenuNumber();

					if (wasScanning)
					{
						// Mode was blinking, hence it needs to be redrawn as it could be in its hidden phase.
						uiUtilityRedrawHeaderOnly(false, false);
					}
					else
					{
						if (((currentMenu == UI_MESSAGE_BOX) && (menuSystemGetPreviousMenuNumber() == UI_PRIVATE_CALL))
								&& (nonVolatileSettings.privateCalls == ALLOW_PRIVATE_CALLS_PTT))
						{
							acceptPrivateCall(uiDataGlobal.receivedPcId, uiDataGlobal.receivedPcTS);
							menuPrivateCallDismiss();
						}
						else if (((currentMenu == MENU_CONTACT_LIST_SUBMENU) || (currentMenu == MENU_CONTACT_QUICKLIST)) && dtmfSequenceIsKeying())
						{
							dtmfSequenceReset();
						}

						// Need to call menuSystemGetCurrentMenuNumber() again, as something has probably
						// changed since last above calls
						if (menuSystemGetCurrentMenuNumber() != UI_MESSAGE_BOX)
						{
							if (currentChannelData->txFreq != 0)
							{
								menuSystemPushNewMenu(UI_TX_SCREEN);
							}
							else
							{
								soundSetMelody(MELODY_NACK_BEEP);
							}
						}
						else
						{
							button_event = EVENT_BUTTON_NONE;
							buttons &= ~BUTTON_PTT;
						}
					}
				}
			}
		}

		ev.function = 0;
		function_event = NO_EVENT;
		keyFunction = NO_EVENT;
		int currentMenu = menuSystemGetCurrentMenuNumber();
		if (KEYCHECK_SHORTUP_NUMBER(keys) && (buttons & BUTTON_SK2) && ((currentMenu == UI_VFO_MODE) || (currentMenu == UI_CHANNEL_MODE)))
		{
			keyFunction = codeplugGetQuickkeyFunctionID(keys.key);
			int menuFunction = QUICKKEY_MENUID(keyFunction);

#if 0 // For demo screen
			if (keys.key == '0')
			{
				static uint8_t demo = 90;
				keyFunction = (UI_HOTSPOT_MODE << 8) | demo; // Hotspot demo mode (able to take screengrabs)

				if (++demo > 99)
				{
					demo = 90;
				}
			}
#endif

#if defined(PLATFORM_RD5R)
			if (keys.key == '5')
			{
				menuFunction = 0;
				keyFunction = FUNC_TOGGLE_TORCH;
				keyboardReset();
			}
			else
			{
#endif
				if ((keyFunction != 0) &&
						((currentMenu == UI_CHANNEL_MODE) || (currentMenu == UI_VFO_MODE) || (currentMenu == menuFunction)))
				{
					if (QUICKKEY_TYPE(keyFunction) == QUICKKEY_MENU)
					{
						bool inChannelMenu;
						bool qkIsValid = true;

						//
						// QuickMenu special cases:
						//
						//   It's permited to share filter quickkeys between Channels and VFO screen.
						//   For this, the itemIndex value needs to be tweaked.
						//
						//   Other QuickMenu entries will simply be ignored if the current Channel/VFO screen doesn't
						//   match the QuickKey menuId.
						//
						if ((inChannelMenu = (currentMenu == UI_CHANNEL_MODE)) || (currentMenu == UI_VFO_MODE))
						{
							// The current QuickKey menu destination doesn't match the current menu (Channel or VFO)
							if (menuFunction == (inChannelMenu ? UI_VFO_QUICK_MENU : UI_CHANNEL_QUICK_MENU))
							{
								int entryId = QUICKKEY_ENTRYID(keyFunction);

								// Convert filters positions
								if ((entryId >= (inChannelMenu ? VFO_SCREEN_QUICK_MENU_FILTER_FM : CH_SCREEN_QUICK_MENU_FILTER_FM))
										&& (entryId <= (inChannelMenu ? VFO_SCREEN_QUICK_MENU_FILTER_DMR_TS : CH_SCREEN_QUICK_MENU_FILTER_DMR_TS)))
								{
									// Apply entry offset to match the filter position on the opposite screen
									if (inChannelMenu)
									{
#if defined(PLATFORM_DM1801)
										entryId -= 1;
#else
										entryId -= 2;
#endif
									}
									else
									{
#if defined(PLATFORM_DM1801)
										entryId += 1;
#else
										entryId += 2;
#endif
									}

									int kf = QUICKKEY_MENUVALUE((inChannelMenu ? UI_CHANNEL_QUICK_MENU : UI_VFO_QUICK_MENU), entryId, QUICKKEY_FUNCTIONID(keyFunction));
									keyFunction = kf;
									menuFunction = (inChannelMenu ? UI_CHANNEL_QUICK_MENU : UI_VFO_QUICK_MENU);
								}
								else
								{
									// We can't use other VFO/Channel QuickMenu entry in a mismatching screen.
									qkIsValid = false;
									keyFunction = NO_EVENT;
								}
							}
						}

						if (qkIsValid)
						{
							if ((menuFunction > 0) && (menuFunction < NUM_MENU_ENTRIES))
							{
								if (currentMenu != menuFunction)
								{
									menuSystemPushNewMenu(menuFunction);

									// Store the beep build by the new menu status. It will be restored after
									// the call of menuSystemCallCurrentMenuTick(), below
									quickkeyPushedMenuMelody = nextKeyBeepMelody;
								}
							}
							ev.function = keyFunction;
							buttons = BUTTON_NONE;
							rotary = 0;
							key_event = EVENT_KEY_NONE;
							button_event = EVENT_BUTTON_NONE;
							rotary_event = EVENT_ROTARY_NONE;
							keys.key = 0;
							keys.event = 0;
							function_event = FUNCTION_EVENT;
						}
						else
						{
							menuFunction = 0;
						}
					}
					else if ((QUICKKEY_TYPE(keyFunction) == QUICKKEY_CONTACT) && (currentMenu != menuFunction))
					{
						int contactIndex = QUICKKEY_CONTACTVALUE(keyFunction);

						if ((contactIndex >= CODEPLUG_CONTACTS_MIN) && (contactIndex <= CODEPLUG_CONTACTS_MAX))
						{
							if (codeplugContactGetDataForIndex(contactIndex, &currentContactData))
							{
								// Use quickkey contact as overrides (contact and its TS, if any)
								menuPrivateCallClear();
								setOverrideTGorPC(currentContactData.tgNumber, (currentContactData.callType == CONTACT_CALLTYPE_PC));

								trxTalkGroupOrPcId = currentContactData.tgNumber;
								if (currentContactData.callType == CONTACT_CALLTYPE_PC)
								{
									trxTalkGroupOrPcId |= (PC_CALL_FLAG << 24);
								}

								// Contact has a TS override set
								if ((currentContactData.reserve1 & CODEPLUG_CONTACT_FLAG_NO_TS_OVERRIDE) == 0x00)
								{
									int ts = ((currentContactData.reserve1 & CODEPLUG_CONTACT_FLAG_TS_OVERRIDE_TIMESLOT_MASK) >> 1);
									trxSetDMRTimeSlot(ts, true);
									tsSetManualOverride(((menuSystemGetRootMenuNumber() == UI_CHANNEL_MODE) ? CHANNEL_CHANNEL : (CHANNEL_VFO_A + nonVolatileSettings.currentVFONumber)), (ts + 1));
								}
								ev.function = FUNC_REDRAW;
								function_event = FUNCTION_EVENT;
							}
						}
					}
				}
				keyboardReset();
#if defined(PLATFORM_RD5R)
			}
#endif
		}
		ev.buttons = buttons;
		ev.keys = keys;
		ev.rotary = rotary;
		ev.events = function_event | (button_event << 1) | (rotary_event << 3) | key_event | (syntheticEvent ? SYNTHETIC_EVENT : 0);
		ev.hasEvent = keyOrButtonChanged || (function_event != NO_EVENT);
		ev.time = ticksGetMillis();


		// Clear the Quickkey slot on SK2 + longdown 0..9 KEY
		if (KEYCHECK_LONGDOWN_NUMBER(ev.keys) && BUTTONCHECK_DOWN(&ev, BUTTON_SK2))
		{
			// Only allow quick keys to be cleared on the 2 main screens
			if (currentMenu == UI_CHANNEL_MODE || currentMenu == UI_VFO_MODE)
			{
				saveQuickkeyMenuLongValue(ev.keys.key, 0, 0);
				soundSetMelody(MELODY_QUICKKEYS_CLEAR_ACK_BEEP);
			}
			else
			{
				soundSetMelody(MELODY_NACK_BEEP);
			}

			// Reset keyboard and event, as this keyboard event HAVE to
			// be ignore by the current menu.
			keyboardReset();
			ev.buttons = BUTTON_NONE;
			ev.keys.event = 0;
			ev.keys.key = 0;
			ev.rotary = 0;
			ev.events = NO_EVENT;
			ev.hasEvent = false;
		}

		menuSystemCallCurrentMenuTick(&ev);

		// Restore the beep built when a menu was pushed by the quickkey above.
		if (quickkeyPushedMenuMelody)
		{
			nextKeyBeepMelody = quickkeyPushedMenuMelody;
			quickkeyPushedMenuMelody = NULL;
			ev.keys.event = KEY_MOD_UP; // Trick to force keyBeepHandler() to set that beep
		}

		// Beep sounds aren't allowed in these modes.
		if (((nonVolatileSettings.audioPromptMode == AUDIO_PROMPT_MODE_SILENT) || voicePromptsIsPlaying()) /*|| (nonVolatileSettings.audioPromptMode == AUDIO_PROMPT_MODE_VOICE)*/)
		{
			if (melody_play != NULL)
			{
				soundStopMelody();
			}

			(void)rxBeepsHandler(); // It will remain silent, only clearing the rxToneState bits.
		}
		else
		{
			if (rxBeepsHandler() == false)
			{
				if ((menuSystemGetCurrentMenuNumber() != UI_SPLASH_SCREEN) &&
						((((key_event == EVENT_KEY_CHANGE) || (button_event == EVENT_BUTTON_CHANGE))
								&& ((buttons & BUTTON_PTT) == 0) && (ev.keys.key != 0))
								|| (function_event == FUNCTION_EVENT)))
				{
					keyBeepHandler(&ev, PTTToggledDown);
				}
			}
		}

		if (uiNotificationHasTimedOut())
		{
			uiNotificationHide(true);
		}

#if !defined(PLATFORM_GD77S)
		// APO checkings
		apoTick((keyOrButtonChanged || (function_event != NO_EVENT) ||
				(settingsIsOptionBitSet(BIT_APO_WITH_RF) ? (getAudioAmpStatus() & AUDIO_AMP_MODE_RF) : false)));

		// Autolock trigger/reset
		if (ticksTimerIsEnabled(&autolockTimer))
		{
			if (((keyOrButtonChanged || (function_event != NO_EVENT)) && (syntheticEvent == false)) || // key event resets the timer
					((currentMenu != UI_CHANNEL_MODE) && (currentMenu != UI_VFO_MODE))) // and could only auto locks while in Channel/VFO menus
			{
				ticksTimerStart(&autolockTimer, (nonVolatileSettings.autolockTimer * 30000U));
			}
			else
			{
				if (ticksTimerHasExpired(&autolockTimer))
				{
					keypadLocked = PTTLocked = true;
					ticksTimerReset(&autolockTimer);
					uiNotificationShow(NOTIFICATION_TYPE_MESSAGE, NOTIFICATION_ID_MESSAGE, 1000, currentLanguage->auto_lock, true);
				}
			}
		}
#endif

		if (((nonVolatileSettings.backlightMode == BACKLIGHT_MODE_AUTO)
				|| (nonVolatileSettings.backlightMode == BACKLIGHT_MODE_BUTTONS)
				|| (nonVolatileSettings.backlightMode == BACKLIGHT_MODE_SQUELCH)) && (menuDataGlobal.lightTimer > 0))
		{
			// Countdown only in (AUTO), (BUTTONS) or (SQUELCH + no audio)
			if ((nonVolatileSettings.backlightMode == BACKLIGHT_MODE_AUTO) || (nonVolatileSettings.backlightMode == BACKLIGHT_MODE_BUTTONS) ||
					((nonVolatileSettings.backlightMode == BACKLIGHT_MODE_SQUELCH) && ((getAudioAmpStatus() & AUDIO_AMP_MODE_RF) == 0)))
			{
				menuDataGlobal.lightTimer--;
			}

			if (menuDataGlobal.lightTimer == 0)
			{
				displayEnableBacklight(false, nonVolatileSettings.displayBacklightPercentageOff);
			}
		}

		voicePromptsTick();
		soundTickMelody();
		voxTick();
		gpsTick();
		aprsBeaconingTick(&ev);
		settingsSaveIfNeeded(false);

		if (((trxTransmissionEnabled || trxIsTransmitting) == false))
		{
#if ! defined(PLATFORM_GD77S)
			// We got a position fix now, start daytime timer
			if (settingsIsOptionBitSet(BIT_AUTO_NIGHT) && (ticksTimerIsEnabled(&daytimeThemeTimer) == false) &&
					(nonVolatileSettings.locationLat != SETTINGS_UNITIALISED_LOCATION_LAT))
			{
				daytimeThemeChangeUpdate(false);
			}

			if (ticksTimerIsEnabled(&daytimeThemeTimer) && ticksTimerHasExpired(&daytimeThemeTimer))
			{
				daytimeThemeTick();

				ticksTimerStart(&daytimeThemeTimer, DAYTIME_THEME_TIMER_INTERVAL);
			}
#endif
		}
		
		while(ticksGetMillis() < startTime + 1)				// ensure this Task runs at 1ms intervals. Regardless of clock speed.
		{
			vTaskDelay(0);
		}

	}
}

void applicationStandbyPowerLoop(void)
{
	batteryRAM_Init();

	uint16_t radioIsInStandby;

	batteryRAM_Read(0, (uint8_t *)&radioIsInStandby, 2);

	if (radioIsInStandby == RADIO_IN_STANDBY_FLAG_PATTERN)
	{
		GPIO_InitTypeDef GPIO_InitStruct = {0};
		RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

		gpsPower(false);  // Turn off the power to the Microphone and GPS

		radioIsInStandby = 0x0000;// do this while the processor is still running at high clock speed

		/* GPIO Ports Clock Enable */
		__HAL_RCC_GPIOD_CLK_ENABLE();
		__HAL_RCC_GPIOE_CLK_ENABLE();

		GPIO_InitStruct.Pin = KEYPAD_COL0_K6_Pin;
		GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
		GPIO_InitStruct.Pull = GPIO_PULLUP;
		HAL_GPIO_Init(KEYPAD_COL0_K6_GPIO_Port, &GPIO_InitStruct);

		GPIO_InitStruct.Pin = KEYPAD_ROW0_K0_Pin;
		GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;
		GPIO_InitStruct.Pull = GPIO_NOPULL;
		GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
		HAL_GPIO_Init(KEYPAD_ROW0_K0_GPIO_Port, &GPIO_InitStruct);

		HAL_GPIO_WritePin(KEYPAD_ROW0_K0_GPIO_Port, KEYPAD_ROW0_K0_Pin, GPIO_PIN_RESET);

		RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK|RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
		RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSE;// Direct HSE

		RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV512;// reduce the clock speed as low as possible
		RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV16;
		RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV16;

		HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0);
		__HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE2);

		__HAL_RCC_GPIOE_CLK_DISABLE();

		while((HAL_GPIO_ReadPin(KEYPAD_COL0_K6_GPIO_Port, KEYPAD_COL0_K6_Pin) == GPIO_PIN_SET));

		// wait for button to be released
		int upCount = 0;
		const int DEBOUNCE_COUNT = 10;// Arbitrary value to define how long the button must be released for, to overcome debounce.
		do
		{
			upCount = 0;
			for (int i = 0; i < DEBOUNCE_COUNT; i++)
			{
				upCount += ((HAL_GPIO_ReadPin(KEYPAD_COL0_K6_GPIO_Port, KEYPAD_COL0_K6_Pin) == GPIO_PIN_SET) ? 1 : 0);
			}
		} while (upCount != DEBOUNCE_COUNT);

		batteryRAM_Write(0, (uint8_t *)&radioIsInStandby, 2);

		NVIC_SystemReset();

		while(true);// wait for reboot
	}
}
