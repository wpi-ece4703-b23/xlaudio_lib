/* xlaudio_lib
   Written by Patrick Schaumont (pschaumont@wpi.edu) at Worcester Polytechnic Institute in 2020

   msp432_boostxl is free software; you can redistribute it and/or modify it under
   the terms of the GNU General Public License as published by the Free
   Software Foundation; either version 3, or (at your option) any later
   version.
   
   msp432_boostxl is distributed in the hope that it will be useful, but WITHOUT ANY
   WARRANTY; without even the implied warranty of MERCHANTABILITY or
   FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
   for more details.
   
   You should have received a copy of the GNU General Public License
   along with msp432_boostxl; see the file COPYING3.  If not see
   <http://www.gnu.org/licenses/>.  */

#include "dac8311.h"
#include <string.h>  // memset and friends
#include "xlaudio.h"

typedef enum {io_poll, io_intr, io_dma} io_enum_t;
io_enum_t glbIO;
uint16_t  glbBUFLEN = 0;

xlaudio_clocksystem_t glbClockSystem = XLAUDIO_DCO;

void xlaudio_clocksystem(xlaudio_clocksystem_t s) {
    glbClockSystem = s;
}

#define MAXPPLEN 128

enum {PING, PONG};

uint16_t           glbPingADC[MAXPPLEN];
uint16_t           glbPongADC[MAXPPLEN];
volatile uint8_t   glbADCPPWrite = PING;
volatile uint8_t   glbADCPPRead  = PING;
volatile uint8_t   glbDACPPWrite = PING;
volatile uint8_t   glbDACPPRead  = PING;

uint16_t  glbPingDAC[MAXPPLEN];
uint16_t  glbPongDAC[MAXPPLEN];
uint16_t* glbActiveDACBuf = glbPongDAC;
uint16_t  glbDACBufIndex  = 0;

xlaudio_sample_process_t glbSampleCallback = 0;
xlaudio_buffer_process_t glbBufferCallback = 0;

#define MIC_POWER_PORT      GPIO_PORT_P4
#define MIC_POWER_PIN       GPIO_PIN1

// use this for the microphone output
#define MIC_INPUT_PORT      GPIO_PORT_P4
#define MIC_INPUT_PIN       GPIO_PIN3
#define MIC_INPUT_CHAN      ADC_INPUT_A10

// use this for pin J1.2 output
#define J1_2_INPUT_PORT     GPIO_PORT_P1
#define J1_2_INPUT_PIN      GPIO_PIN2
#define J1_2_INPUT_CHAN     ADC_INPUT_A15

void blockingerror();

void initMic(xlaudio_in_enum_t input) {
    if (input == XLAUDIO_MIC_IN) {
        GPIO_setAsPeripheralModuleFunctionInputPin(MIC_INPUT_PORT,
                                                   MIC_INPUT_PIN,
                                                   GPIO_TERTIARY_MODULE_FUNCTION); // analog pin
    } else if (input == XLAUDIO_J1_2_IN) {
        GPIO_setAsPeripheralModuleFunctionInputPin(J1_2_INPUT_PORT,
                                                   J1_2_INPUT_PIN,
                                                   GPIO_TERTIARY_MODULE_FUNCTION); // analog pin
    } else blockingerror();
    GPIO_setAsOutputPin   (MIC_POWER_PORT, MIC_POWER_PIN);
    GPIO_setOutputLowOnPin(MIC_POWER_PORT, MIC_POWER_PIN);
}

void initSwitch() {
    GPIO_setAsInputPin   (MIC_POWER_PORT, MIC_POWER_PIN);
}

void micOn(void) {
    GPIO_setOutputHighOnPin(MIC_POWER_PORT, MIC_POWER_PIN);
}

void micOff(void) {
    GPIO_setOutputLowOnPin(MIC_POWER_PORT, MIC_POWER_PIN);
}

#define AUDIO_AMP_EN_PORT   GPIO_PORT_P5
#define AUDIO_AMP_EN_PIN    GPIO_PIN0

void initAmp(void) {
    GPIO_setAsOutputPin(AUDIO_AMP_EN_PORT, AUDIO_AMP_EN_PIN);
    GPIO_setOutputLowOnPin(AUDIO_AMP_EN_PORT, AUDIO_AMP_EN_PIN);
}

void initClockXTAL() {
    // high frequency clock source + prepare system for 48MHz operation
    GPIO_setAsPeripheralModuleFunctionOutputPin(GPIO_PORT_PJ, GPIO_PIN3, GPIO_PRIMARY_MODULE_FUNCTION);
    GPIO_setAsPeripheralModuleFunctionOutputPin(GPIO_PORT_PJ, GPIO_PIN2, GPIO_PRIMARY_MODULE_FUNCTION);

    GPIO_setAsPeripheralModuleFunctionOutputPin(GPIO_PORT_PJ, GPIO_PIN0, GPIO_PRIMARY_MODULE_FUNCTION);
    GPIO_setAsPeripheralModuleFunctionOutputPin(GPIO_PORT_PJ, GPIO_PIN1, GPIO_PRIMARY_MODULE_FUNCTION);

    CS_setExternalClockSourceFrequency(32000, 48000000);

    PCM_setCoreVoltageLevel(PCM_VCORE1);
    FlashCtl_setWaitState  (FLASH_BANK0, 2);
    FlashCtl_setWaitState  (FLASH_BANK1, 2);
    CS_startHFXT(false);
    CS_startLFXT(CS_LFXT_DRIVE3);

    CS_initClockSignal(CS_MCLK,   CS_HFXTCLK_SELECT,  CS_CLOCK_DIVIDER_1);
    CS_initClockSignal(CS_ACLK,   CS_LFXTCLK_SELECT,  CS_CLOCK_DIVIDER_1);
    CS_initClockSignal(CS_HSMCLK, CS_HFXTCLK_SELECT,  CS_CLOCK_DIVIDER_1);
    CS_initClockSignal(CS_SMCLK,  CS_HFXTCLK_SELECT,  CS_CLOCK_DIVIDER_1);
    CS_initClockSignal(CS_BCLK,   CS_LFXTCLK_SELECT,  CS_CLOCK_DIVIDER_1);
}

void initClock() {
    // high frequency clock source + prepare system for 48MHz operation
    PCM_setCoreVoltageLevel(PCM_VCORE1);
    FlashCtl_setWaitState  (FLASH_BANK0, 2);
    FlashCtl_setWaitState  (FLASH_BANK1, 2);

    CS_setDCOCenteredFrequency(CS_DCO_FREQUENCY_48);
    CS_initClockSignal(CS_MCLK,   CS_DCOCLK_SELECT,   CS_CLOCK_DIVIDER_1);
    CS_initClockSignal(CS_ACLK,   CS_REFOCLK_SELECT,  CS_CLOCK_DIVIDER_1);
    CS_initClockSignal(CS_HSMCLK, CS_DCOCLK_SELECT,   CS_CLOCK_DIVIDER_1);
    CS_initClockSignal(CS_SMCLK,  CS_DCOCLK_SELECT,   CS_CLOCK_DIVIDER_1);
    CS_initClockSignal(CS_BCLK,   CS_REFOCLK_SELECT,  CS_CLOCK_DIVIDER_1);
}

void debugpininit() {
    GPIO_setAsOutputPin( GPIO_PORT_P3, GPIO_PIN5);
    GPIO_setOutputLowOnPin( GPIO_PORT_P3, GPIO_PIN5);
}

void xlaudio_debugpinhigh() {
    GPIO_setOutputHighOnPin( GPIO_PORT_P3, GPIO_PIN5);
}

void xlaudio_debugpinlow() {
    GPIO_setOutputLowOnPin( GPIO_PORT_P3, GPIO_PIN5);
}

void dutypininit() {
    GPIO_setAsOutputPin( GPIO_PORT_P5, GPIO_PIN7);
    GPIO_setOutputLowOnPin( GPIO_PORT_P5, GPIO_PIN7);
}
void dutypinhigh() {
    GPIO_setOutputHighOnPin( GPIO_PORT_P5, GPIO_PIN7);
}

void dutypinlow() {
    GPIO_setOutputLowOnPin( GPIO_PORT_P5, GPIO_PIN7);
}

void errorledinit() {
    GPIO_setAsOutputPin( GPIO_PORT_P1, GPIO_PIN0);
    GPIO_setOutputLowOnPin( GPIO_PORT_P1, GPIO_PIN0);
}

void xlaudio_errorledon() {
    GPIO_setOutputHighOnPin( GPIO_PORT_P1, GPIO_PIN0);
}

void xlaudio_errorledoff() {
    GPIO_setOutputLowOnPin( GPIO_PORT_P1, GPIO_PIN0);
}

void colorledinit() {
    GPIO_setAsOutputPin( GPIO_PORT_P2, GPIO_PIN0);
    GPIO_setOutputLowOnPin( GPIO_PORT_P2, GPIO_PIN0);
    GPIO_setAsOutputPin( GPIO_PORT_P2, GPIO_PIN1);
    GPIO_setOutputLowOnPin( GPIO_PORT_P2, GPIO_PIN1);
    GPIO_setAsOutputPin( GPIO_PORT_P2, GPIO_PIN2);
    GPIO_setOutputLowOnPin( GPIO_PORT_P2, GPIO_PIN2);
}

void xlaudio_colorledred() {
    GPIO_setOutputHighOnPin ( GPIO_PORT_P2, GPIO_PIN0);
    GPIO_setOutputLowOnPin  ( GPIO_PORT_P2, GPIO_PIN1);
    GPIO_setOutputLowOnPin  ( GPIO_PORT_P2, GPIO_PIN2);
}

void xlaudio_colorledgreen() {
    GPIO_setOutputLowOnPin  ( GPIO_PORT_P2, GPIO_PIN0);
    GPIO_setOutputHighOnPin ( GPIO_PORT_P2, GPIO_PIN1);
    GPIO_setOutputLowOnPin  ( GPIO_PORT_P2, GPIO_PIN2);
}

void xlaudio_colorledblue() {
    GPIO_setOutputLowOnPin  ( GPIO_PORT_P2, GPIO_PIN0);
    GPIO_setOutputLowOnPin  ( GPIO_PORT_P2, GPIO_PIN1);
    GPIO_setOutputHighOnPin ( GPIO_PORT_P2, GPIO_PIN2);
}

void xlaudio_colorledoff() {
    GPIO_setOutputLowOnPin  ( GPIO_PORT_P2, GPIO_PIN0);
    GPIO_setOutputLowOnPin  ( GPIO_PORT_P2, GPIO_PIN1);
    GPIO_setOutputLowOnPin  ( GPIO_PORT_P2, GPIO_PIN2);
}

void initPushButton() {
    GPIO_setAsInputPinWithPullUpResistor (GPIO_PORT_P1, GPIO_PIN1);
    GPIO_setAsInputPinWithPullUpResistor (GPIO_PORT_P1, GPIO_PIN4);
}

int xlaudio_pushButtonLeftDown() {
    return (GPIO_getInputPinValue(GPIO_PORT_P1, GPIO_PIN1) == 0);
}

int xlaudio_pushButtonLeftUp() {
    return (GPIO_getInputPinValue(GPIO_PORT_P1, GPIO_PIN1) != 0);
}

int xlaudio_pushButtonRightDown() {
    return (GPIO_getInputPinValue(GPIO_PORT_P1, GPIO_PIN4) == 0);
}

int xlaudio_pushButtonRightUp() {
    return (GPIO_getInputPinValue(GPIO_PORT_P1, GPIO_PIN4) != 0);
}

void blockingerror() {
    volatile int i;
    while (1) {
        xlaudio_errorledon();
        for (i=0; i<500000; i++) ;
        xlaudio_errorledoff();
        for (i=0; i<500000; i++) ;
    }
}

Timer_A_PWMConfig glbPWMConfig;

void configureSampleclock(fs_enum_t fs) {
    uint_fast16_t ccrvalue[9] = {
          6000,
          5000,
          4354,
          3000,
          2177,
          2000,
          1500,
          1088,
          1000
    };

    glbPWMConfig.clockSource = TIMER_A_CLOCKSOURCE_SMCLK;
    glbPWMConfig.clockSourceDivider = TIMER_A_CLOCKSOURCE_DIVIDER_1;
    glbPWMConfig.timerPeriod = ccrvalue[fs];
    glbPWMConfig.compareRegister = TIMER_A_CAPTURECOMPARE_REGISTER_1;
    glbPWMConfig.compareOutputMode = TIMER_A_OUTPUTMODE_SET_RESET;
    glbPWMConfig.dutyCycle = ccrvalue[fs]/2;

    Timer_A_generatePWM(TIMER_A0_BASE, &glbPWMConfig);

    if (glbIO == io_dma) {
        Timer_A_enableCaptureCompareInterrupt(TIMER_A0_BASE,TIMER_A_CAPTURECOMPARE_REGISTER_1);
        Interrupt_enableInterrupt(INT_TA0_N);
    }
}

void configureBuffer(buflen_enum_t      _pplen) {

   uint16_t buflen[] = {8, 16, 32, 64, 128};
   uint16_t k;

   glbBUFLEN = buflen[_pplen];

   for (k=0; k<glbBUFLEN; k++) {
       glbPingDAC[k] = 0;
       glbPongDAC[k] = 0;
       glbPingADC[k] = 0;
       glbPongADC[k] = 0;
   }

}

void startSampleClock() {
    Timer_A_clearTimer(TIMER_A0_BASE);
    Timer_A_startCounter(TIMER_A0_BASE, TIMER_A_UP_MODE);
}

void stopSampleClock() {
    Timer_A_stopTimer(TIMER_A0_BASE);
}

static DMA_ControlTable  dmaControlTable[32];

void initADC(xlaudio_in_enum_t  _audioin) {

    ADC14_enableModule();
    ADC14_initModule(ADC_CLOCKSOURCE_MCLK, // 48MHz conversion rate
                     ADC_PREDIVIDER_1,
                     ADC_DIVIDER_1,
                     0);

    if (glbIO == io_poll) {

        ADC14_enableSampleTimer(ADC_MANUAL_ITERATION);

    } else if ((glbIO == io_dma) || (glbIO == io_intr)) {

        ADC14_setSampleHoldTrigger(ADC_TRIGGER_SOURCE1, false); // conversion triggered by timer

    }

    ADC14_configureSingleSampleMode(ADC_MEM0, true);
    if (_audioin == XLAUDIO_MIC_IN)
        ADC14_configureConversionMemory(ADC_MEM0,
                                        ADC_VREFPOS_AVCC_VREFNEG_VSS,
                                        MIC_INPUT_CHAN,
                                        ADC_NONDIFFERENTIAL_INPUTS);
    else if (_audioin == XLAUDIO_J1_2_IN)
        ADC14_configureConversionMemory(ADC_MEM0,
                                        ADC_VREFPOS_AVCC_VREFNEG_VSS,
                                        J1_2_INPUT_CHAN,
                                        ADC_NONDIFFERENTIAL_INPUTS);
    else blockingerror();

    ADC14_setResolution(ADC_14BIT);        // 16 cycle conversion time

    if (glbIO == io_intr) {

        ADC14_enableInterrupt(ADC_INT0);
        Interrupt_enableInterrupt(INT_ADC14);

    }

    if (glbIO == io_dma) {

        DMA_enableModule();
        DMA_setControlBase(dmaControlTable);

        DMA_disableChannelAttribute(DMA_CH7_ADC14, (UDMA_ATTR_ALTSELECT | UDMA_ATTR_USEBURST | UDMA_ATTR_HIGH_PRIORITY | UDMA_ATTR_REQMASK));

        DMA_setChannelControl(DMA_CH7_ADC14 | UDMA_PRI_SELECT, (UDMA_SIZE_16 | UDMA_SRC_INC_NONE | UDMA_DST_INC_16 | UDMA_ARB_1));
        DMA_setChannelTransfer(DMA_CH7_ADC14 | UDMA_PRI_SELECT, UDMA_MODE_PINGPONG, (void*) &ADC14->MEM[0], (void *) (glbPingADC), glbBUFLEN);

        DMA_setChannelControl(DMA_CH7_ADC14 | UDMA_ALT_SELECT, (UDMA_SIZE_16 | UDMA_SRC_INC_NONE | UDMA_DST_INC_16 | UDMA_ARB_1));
        DMA_setChannelTransfer(DMA_CH7_ADC14 | UDMA_ALT_SELECT, UDMA_MODE_PINGPONG, (void*) &ADC14->MEM[0], (void *) (glbPongADC), glbBUFLEN);
        glbADCPPWrite   = PING;
        glbADCPPRead    = PONG;
        glbDACPPWrite   = PING;
        glbDACPPRead    = PONG;
        glbActiveDACBuf = glbPongDAC;
        glbDACBufIndex  = 0;

        DMA_assignInterrupt(DMA_INT1, DMA_CHANNEL_7);
        Interrupt_enableInterrupt(INT_DMA_INT1);
        DMA_assignChannel(DMA_CH7_ADC14);

    }
}

void TA0_N_IRQHandler (void) {
    Timer_A_clearCaptureCompareInterrupt(TIMER_A0_BASE, TIMER_A_CAPTURECOMPARE_REGISTER_1);

    if (glbIO == io_intr) {

        DAC8311_updateDacOut(glbPingDAC[0]);

    } else if (glbIO == io_dma) {

        DAC8311_updateDacOut(glbActiveDACBuf[glbDACBufIndex++]);

        if (glbDACBufIndex == glbBUFLEN) {
            glbDACBufIndex = 0;
            if (glbDACPPWrite == PING) {
                dutypinhigh();
               glbActiveDACBuf = glbPingDAC;
            } else {
                dutypinlow();
              glbActiveDACBuf = glbPongDAC;
            }
        }

    } else

        blockingerror();
}

void DMA_INT1_IRQHandler(void) {
    if(DMA_getChannelAttribute(7) & UDMA_ATTR_ALTSELECT) {
        DMA_setChannelControl(DMA_CH7_ADC14 | UDMA_PRI_SELECT, (UDMA_SIZE_16 | UDMA_SRC_INC_NONE | UDMA_DST_INC_16 | UDMA_ARB_1));
        DMA_setChannelTransfer(DMA_CH7_ADC14 | UDMA_PRI_SELECT, UDMA_MODE_PINGPONG, (void*) &ADC14->MEM[0], (void *) (glbPingADC), glbBUFLEN);
        glbADCPPWrite = PING;
    } else {
        DMA_setChannelControl(DMA_CH7_ADC14 | UDMA_ALT_SELECT, (UDMA_SIZE_16 | UDMA_SRC_INC_NONE | UDMA_DST_INC_16 | UDMA_ARB_1));
        DMA_setChannelTransfer(DMA_CH7_ADC14 | UDMA_ALT_SELECT, UDMA_MODE_PINGPONG, (void*) &ADC14->MEM[0], (void *) (glbPongADC), glbBUFLEN);
        glbADCPPWrite = PONG;
    }
}

void DMA_ERR_IRQHandler(void) {
    uint32_t ui32Status;

    ui32Status = MAP_DMA_getErrorStatus();
    if (ui32Status) {
        DMA_clearErrorStatus();
    }
}

void ADC14_IRQHandler(void) {
    uint64_t status;
    static int recursive = 0;

    if (recursive) xlaudio_errorledon();

    status = ADC14_getEnabledInterruptStatus();
    ADC14_clearInterruptFlag(status);

    if (status & ADC_INT0) {
        glbPingADC[0] = ADC14_getResult(ADC_MEM0);

        recursive = 1;
        dutypinhigh();
        glbPingDAC[0] = glbSampleCallback(glbPingADC[0]);
        dutypinlow();
        recursive = 0;

        DAC8311_updateDacOut(glbPingDAC[0]);
    }

}

void initTimer32() {
    Timer32_initModule(TIMER32_0_BASE,          // fpr performance evaluation
                       TIMER32_PRESCALER_1,
                       TIMER32_32BIT,
                       TIMER32_FREE_RUN_MODE);
    Timer32_initModule(TIMER32_1_BASE,          // for software delays
                       TIMER32_PRESCALER_1,
                       TIMER32_32BIT,
                       TIMER32_FREE_RUN_MODE);
}

void xlaudio_init() {
    initTimer32();
    dutypininit();
    debugpininit();
    errorledinit();
    colorledinit();
    initPushButton();
    if (glbClockSystem == XLAUDIO_XTAL)
        initClockXTAL();
    else
        initClock();
}

void xlaudio_init_poll(xlaudio_in_enum_t  _audioin,
                        xlaudio_sample_process_t _cb
                        ) {
    glbIO = io_poll;
    glbBUFLEN = 1;
    glbSampleCallback = _cb;

    initTimer32();
    dutypininit();
    debugpininit();
    errorledinit();
    colorledinit();
    initPushButton();
    if (glbClockSystem == XLAUDIO_XTAL)
        initClockXTAL();
    else
        initClock();

    initAmp();
    initMic(_audioin);
    micOn();

    DAC8311_init();
    initADC(_audioin);
}

void xlaudio_init_intr(fs_enum_t          _fs,
                       xlaudio_in_enum_t  _audioin,
                       xlaudio_sample_process_t _cb
                       ) {
    glbIO = io_intr;
    glbBUFLEN = 1;
    glbSampleCallback = _cb;

    initTimer32();
    dutypininit();
    debugpininit();
    errorledinit();
    colorledinit();
    initPushButton();
    if (glbClockSystem == XLAUDIO_XTAL)
        initClockXTAL();
    else
        initClock();

    initAmp();
    initMic(_audioin);
    micOn();

    DAC8311_init();
    configureSampleclock(_fs);
    initADC(_audioin);
}

void xlaudio_init_dma (fs_enum_t          _fs,
                       xlaudio_in_enum_t  _audioin,
                       buflen_enum_t      _pplen,
                       xlaudio_buffer_process_t _cb
                       ) {
    glbIO = io_dma;
    configureBuffer(_pplen);
    glbBufferCallback = _cb;

    initTimer32();
    dutypininit();
    debugpininit();
    errorledinit();
    colorledinit();
    initPushButton();
    if (glbClockSystem == XLAUDIO_XTAL)
        initClockXTAL();
    else
        initClock();

    initAmp();
    initMic(_audioin);
    micOn();

    DAC8311_init();
    configureSampleclock(_fs);
    initADC(_audioin);
}

void xlaudio_run() {

    if (glbIO == io_poll) {

        ADC14_enableConversion();
        while (1) {
            ADC14_toggleConversionTrigger();
            while (ADC14_isBusy()) ;
            glbPingADC[0] = ADC14_getResult(ADC_MEM0);

            dutypinhigh();
            glbPingDAC[0] = glbSampleCallback(glbPingADC[0]);
            dutypinlow();

            DAC8311_updateDacOut(glbPingDAC[0]);
         }

    } else if (glbIO == io_intr) {

        Interrupt_enableMaster();
        ADC14_enableConversion();
        startSampleClock();

        while (1)
            PCM_gotoLPM0();

    } else if (glbIO == io_dma) {

        Interrupt_enableMaster();
        DMA_enableChannel(7);
        ADC14_enableConversion();
        startSampleClock();

        glbADCPPRead = PING;

        while (1) {

            if ((glbADCPPWrite == PING) & (glbADCPPRead == PONG)) {

                glbBufferCallback(glbPingADC, glbPingDAC);
                glbADCPPRead = PING;  // ADC PING BUFFER HAS BEEN READ
                glbDACPPWrite = PING; // DAC PING BUFFER HAS FILLED UP

            } else if ((glbADCPPWrite == PONG) & (glbADCPPRead == PING)) {

                glbBufferCallback(glbPongADC, glbPongDAC);
                glbADCPPRead = PONG;   // ADC PONG BUFFER HAS BEEN READ
                glbDACPPWrite = PONG;  // DAC PONG BUFFER HAS FILLED UP

            }

        }

    } else
        blockingerror();
}

#include <stdlib.h>

void stopPerf() {
    Timer32_haltTimer(TIMER32_0_BASE);
}

uint32_t perfLap() {
    static unsigned int previousSnap;
    unsigned int currentSnap, ret;
    currentSnap = Timer32_getValue(TIMER32_0_BASE);
    ret = (previousSnap - currentSnap);
    previousSnap = currentSnap;
    return ret;
}

#define N_MEASUREMENTS 11

int comp_uint32(const void *a, const void *b) {
    return (*(uint32_t *)a - *(uint32_t *)b);
}

uint32_t median(uint32_t arr[N_MEASUREMENTS]) {
    qsort (arr, N_MEASUREMENTS, sizeof(uint32_t), comp_uint32);
    return arr[N_MEASUREMENTS/2];
}

void xlaudio_delay(uint32_t cycles) {
    uint32_t v;
    Timer32_setCount(TIMER32_1_BASE, cycles);
    Timer32_startTimer(TIMER32_1_BASE, false);
    do {
        v = Timer32_getValue(TIMER32_1_BASE);
    } while (v < cycles);
    Timer32_haltTimer(TIMER32_1_BASE);
}

uint32_t xlaudio_measurePerfSample(xlaudio_sample_process_t _cb) {
    uint32_t cycles[N_MEASUREMENTS];
    uint32_t overhead[N_MEASUREMENTS];
    uint32_t k;

    Timer32_startTimer(TIMER32_0_BASE, false);

    for (k = 0; k < N_MEASUREMENTS; k++) {
        perfLap();
        overhead[k] = perfLap();
    }

    volatile uint16_t sample_in;
    volatile uint16_t sample_out;
    for (k = 0; k < N_MEASUREMENTS; k++) {
        perfLap();
        sample_out = _cb(sample_in);
        cycles[k] = perfLap();
    }

    return median(cycles) - median(overhead);
}

uint32_t xlaudio_measurePerfBuffer(xlaudio_buffer_process_t _cb) {
    uint32_t cycles[N_MEASUREMENTS];
    uint32_t overhead[N_MEASUREMENTS];
    uint32_t k;

    Timer32_startTimer(TIMER32_0_BASE, false);

    for (k = 0; k < N_MEASUREMENTS; k++) {
        perfLap();
        overhead[k] = perfLap();
    }

    if (glbBUFLEN == 0)
        blockingerror();

    memset(glbPingADC,  0, MAXPPLEN);
    memset(glbPingDAC, 0, MAXPPLEN);
    for (k = 0; k < N_MEASUREMENTS; k++) {
        perfLap();
        _cb(glbPingADC, glbPingDAC);
        cycles[k] = perfLap();
    }

    return median(cycles) - median(overhead);
}
