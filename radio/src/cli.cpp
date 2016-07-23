/*
 * Copyright (C) OpenTX
 *
 * Based on code named
 *   th9x - http://code.google.com/p/th9x
 *   er9x - http://code.google.com/p/er9x
 *   gruvin9x - http://code.google.com/p/gruvin9x
 *
 * License GPLv2: http://www.gnu.org/licenses/gpl-2.0.html
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "opentx.h"
#include "diskio.h"
#include <ctype.h>
#include <malloc.h>

#define CLI_COMMAND_MAX_ARGS           8
#define CLI_COMMAND_MAX_LEN            256

OS_TID cliTaskId;
TaskStack<CLI_STACK_SIZE> _ALIGNED(8) cliStack; // stack must be aligned to 8 bytes otherwise printf for %f does not work!
Fifo<uint8_t, 256> cliRxFifo;
uint8_t cliTracesEnabled = true;
// char cliLastLine[CLI_COMMAND_MAX_LEN+1];

typedef int (* CliFunction) (const char ** args);
int cliExecLine(char * line);
int cliExecCommand(const char ** argv);
int cliHelp(const char ** argv);

struct CliCommand
{
  const char * name;
  CliFunction func;
  const char * args;
};

struct MemArea
{
  const char * name;
  void * start;
  int size;
};

void cliPrompt()
{
  serialPutc('>');
}

int toLongLongInt(const char ** argv, int index, long long int * val)
{
  if (*argv[index] == '\0') {
    return 0;
  }
  else {
    int base = 10;
    const char * s = argv[index];
    if (strlen(s) > 2 && s[0] == '0' && s[1] == 'x') {
      base = 16;
      s = &argv[index][2];
    }
    char * endptr = NULL;
    *val = strtoll(s, &endptr, base);
    if (*endptr == '\0')
      return 1;
    else {
      serialPrint("%s: Invalid argument \"%s\"", argv[0], argv[index]);
      return -1;
    }
  }
}

int toInt(const char ** argv, int index, int * val)
{
  long long int lval = 0;
  int result = toLongLongInt(argv, index, &lval);
  *val = (int)lval;
  return result;
}

int cliBeep(const char ** argv)
{
  int freq = BEEP_DEFAULT_FREQ;
  int duration = 100;
  if (toInt(argv, 1, &freq) >= 0 && toInt(argv, 2, &duration) >= 0) {
    audioQueue.playTone(freq, duration, 20, PLAY_NOW);
  }
  return 0;
}

int cliPlay(const char ** argv)
{
  audioQueue.playFile(argv[1], PLAY_NOW);
  return 0;
}

int cliLs(const char ** argv)
{
  FILINFO fno;
  DIR dir;
  char *fn;   /* This function is assuming non-Unicode cfg. */
#if _USE_LFN
  TCHAR lfn[_MAX_LFN + 1];
  fno.lfname = lfn;
  fno.lfsize = sizeof(lfn);
#endif

  FRESULT res = f_opendir(&dir, argv[1]);        /* Open the directory */
  if (res == FR_OK) {
    for (;;) {
      res = f_readdir(&dir, &fno);                   /* Read a directory item */
      if (res != FR_OK || fno.fname[0] == 0) break;  /* Break on error or end of dir */

#if _USE_LFN
      fn = *fno.lfname ? fno.lfname : fno.fname;
#else
      fn = fno.fname;
#endif
      serialPrint(fn);
    }
  }
  else {
    serialPrint("%s: Invalid directory \"%s\"", argv[0], argv[1]);
  }
  return 0;
}

int cliRead(const char ** argv)
{
  FIL file;
  uint32_t bytesRead = 0;
  int bufferSize;
  if (toInt(argv, 2, &bufferSize) == 0 || bufferSize < 0 ) {
    serialPrint("%s: Invalid buffer size \"%s\"", argv[0], argv[2]);
    return 0;
  }

  uint8_t * buffer = (uint8_t*) malloc(bufferSize);
  FRESULT result = f_open(&file, argv[1], FA_OPEN_EXISTING | FA_READ);
  if (result != FR_OK) {
    free(buffer);
    serialPrint("%s: File not found \"%s\"", argv[0], argv[1]);
    return 0;
  }

  tmr10ms_t start = get_tmr10ms();

  while (true) {
    UINT read;
    result = f_read(&file, buffer, sizeof(buffer), &read);
    if (result == FR_OK) {
      if (read == 0) {
        // end of file
        f_close(&file);
        break;
      }
      bytesRead += read;
    }
  }
  uint32_t elapsedTime = (get_tmr10ms() - start) * 10;
  if (elapsedTime == 0) elapsedTime = 1;
  uint32_t speed = bytesRead / elapsedTime;
  serialPrint("Read %d bytes in %d ms, speed %d kB/s", bytesRead, elapsedTime, speed);
  free(buffer);
  return 0;
}

int cliReadSD(const char ** argv)
{
  int startSector;
  int numberOfSectors;
  int bufferSectors;
  if (toInt(argv, 1, &startSector) == 0 || startSector < 0 ) {
    serialPrint("%s: Invalid start sector \"%s\"", argv[0], argv[1]);
    return 0;
  }
  if (toInt(argv, 2, &numberOfSectors) == 0 || numberOfSectors < 0 ) {
    serialPrint("%s: Invalid number of sectors \"%s\"", argv[0], argv[2]);
    return 0;
  }

  if (toInt(argv, 3, &bufferSectors) == 0 || bufferSectors < 0 ) {
    serialPrint("%s: Invalid number of buffrer sectors \"%s\"", argv[0], argv[3]);
    return 0;
  }

  uint8_t * buffer = (uint8_t*) malloc(512*bufferSectors);

  uint32_t bytesRead = numberOfSectors * 512;
  tmr10ms_t start = get_tmr10ms();

  while (numberOfSectors > 0) {
    DRESULT res = disk_read(0, buffer, startSector, bufferSectors);
    if (res != RES_OK) {
      serialPrint("disk_read error: %d", res);
      free(buffer);
      return 0;
    }
    if (numberOfSectors >= bufferSectors) {
      numberOfSectors -= bufferSectors;
    }
    else {
      numberOfSectors = 0;
    }
  }

  uint32_t elapsedTime = (get_tmr10ms() - start) * 10;
  if (elapsedTime == 0) elapsedTime = 1;
  uint32_t speed = bytesRead / elapsedTime;
  serialPrint("Read %d bytes in %d ms, speed %d kB/s", bytesRead, elapsedTime, speed);
  free(buffer);
  return 0;
}


int cliTrace(const char ** argv)
{
  if (!strcmp(argv[1], "on")) {
    cliTracesEnabled = true;
  }
  else if (!strcmp(argv[1], "off")) {
    cliTracesEnabled = false;
  }
  else {
    serialPrint("%s: Invalid argument \"%s\"", argv[0], argv[1]);
  }
  return 0;
}

int cliStackInfo(const char ** argv)
{
  serialPrint("[MAIN] %d available / %d", stackAvailable(), stackSize() * 4);  // stackSize() returns size in 32bit chunks
  serialPrint("[MENUS] %d available / %d", menusStack.available(), menusStack.size());
  serialPrint("[MIXER] %d available / %d", mixerStack.available(), mixerStack.size());
  serialPrint("[AUDIO] %d available / %d", audioStack.available(), audioStack.size());
  serialPrint("[CLI] %d available / %d", cliStack.available(), cliStack.size());
  return 0;
}

int cliMemoryInfo(const char ** argv)
{
  // struct mallinfo {
  //   int arena;    /* total space allocated from system */
  //   int ordblks;  /* number of non-inuse chunks */
  //   int smblks;   /* unused -- always zero */
  //   int hblks;    /* number of mmapped regions */
  //   int hblkhd;   /* total space in mmapped regions */
  //   int usmblks;  /* unused -- always zero */
  //   int fsmblks;  /* unused -- always zero */
  //   int uordblks; /* total allocated space */
  //   int fordblks; /* total non-inuse space */
  //   int keepcost; /* top-most, releasable (via malloc_trim) space */
  // };
  struct mallinfo info = mallinfo();
  serialPrint("arena %d", info.arena);
  serialPrint("ordblks %d", info.ordblks);
  serialPrint("uordblks %d", info.uordblks);
  serialPrint("fordblks %d", info.fordblks);
  serialPrint("keepcost %d", info.keepcost);
  return 0;
}

int cliReboot(const char ** argv)
{
#if !defined(SIMU)
  if (!strcmp(argv[1], "wdt")) {
    // do a user requested watchdog test by pausing mixer thread
    pausePulses();
  }
  else {
    NVIC_SystemReset();
  }
#endif
  return 0;
}

#if defined(PCBFLAMENCO)
int cliReadBQ24195(const char ** argv)
{
  int index = 0;
  if (toInt(argv, 1, &index) > 0) {
    serialPrint("BQ24195[%d] = 0x%02x", index, i2cReadBQ24195(index));
  }
  else {
    serialPrint("%s: Invalid arguments \"%s\" \"%s\"", argv[0], argv[1]);
  }
  return 0;
}

int cliWriteBQ24195(const char ** argv)
{
  int index = 0;
  int data = 0;
  if (toInt(argv, 1, &index) > 0 && toInt(argv, 2, &data) > 0) {
    i2cWriteBQ24195(index, data);
  }
  else {
    serialPrint("%s: Invalid arguments \"%s\" \"%s\"", argv[0], argv[1], argv[2]);
  }
  return 0;
}
#endif

const MemArea memAreas[] = {
  { "RCC", RCC, sizeof(RCC_TypeDef) },
  { "GPIOA", GPIOA, sizeof(GPIO_TypeDef) },
  { "GPIOB", GPIOB, sizeof(GPIO_TypeDef) },
  { "GPIOC", GPIOC, sizeof(GPIO_TypeDef) },
  { "GPIOD", GPIOD, sizeof(GPIO_TypeDef) },
  { "GPIOE", GPIOE, sizeof(GPIO_TypeDef) },
  { "GPIOF", GPIOF, sizeof(GPIO_TypeDef) },
  { "GPIOG", GPIOG, sizeof(GPIO_TypeDef) },
  { "USART1", USART1, sizeof(USART_TypeDef) },
  { "USART2", USART2, sizeof(USART_TypeDef) },
  { "USART3", USART3, sizeof(USART_TypeDef) },
  { NULL, NULL, 0 },
};

int cliSet(const char ** argv)
{
  if (!strcmp(argv[1], "rtc")) {
    struct gtm t;
    int year, month, day, hour, minute, second;
    if (toInt(argv, 2, &year) > 0 && toInt(argv, 3, &month) > 0 && toInt(argv, 4, &day) > 0 && toInt(argv, 5, &hour) > 0 && toInt(argv, 6, &minute) > 0 && toInt(argv, 7, &second) > 0) {
      t.tm_year = year-1900;
      t.tm_mon = month-1;
      t.tm_mday = day;
      t.tm_hour = hour;
      t.tm_min = minute;
      t.tm_sec = second;
      g_rtcTime = gmktime(&t); // update local timestamp and get wday calculated
      rtcSetTime(&t);
    }
    else {
      serialPrint("%s: Invalid arguments \"%s\" \"%s\"", argv[0], argv[1], argv[2]);
    }
  }
  else if (!strcmp(argv[1], "volume")) {
    int level = 0;
    if (toInt(argv, 2, &level) > 0) {
      setVolume(level);
    }
    else {
      serialPrint("%s: Invalid argument \"%s\" \"%s\"", argv[0], argv[1], argv[2]);
    }
    return 0;
  }
  return 0;
}


#if defined(DEBUG_INTERRUPTS)
void printInterrupts()
{
  __disable_irq();
  struct InterruptCounters ic = interruptCounters;
  memset(&interruptCounters, 0, sizeof(interruptCounters));
  interruptCounters.resetTime = get_tmr10ms();
  __enable_irq();
  serialPrint("Interrupts count in the last %u ms:", (get_tmr10ms() - ic.resetTime) * 10);
  for(int n = 0; n < INT_LAST; n++) {
    serialPrint("%s: %u", interruptNames[n], ic.cnt[n]);
  }
}
#endif //#if defined(DEBUG_INTERRUPTS)

#if defined(DEBUG_TASKS)

void printTaskSwitchLog()
{
  serialPrint("Tasks legend [<task_id>, <task name>]:");
  for(int n = 0; n <= CFG_MAX_USER_TASKS+1; n++) {
    if (0 == n) {
      serialPrint("%d: Idle", n);
    }
    if (cliTaskId == n) {
      serialPrint("%d: CLI", n);
    }
    else if (menusTaskId == n) {
      serialPrint("%d: menus", n);
    }
    else if (mixerTaskId == n) {
      serialPrint("%d: mixer", n);
    }
    else if (audioTaskId == n) {
      serialPrint("%d: audio", n);
    }
#if defined(BLUETOOTH)
    else if (btTaskId == n) {
      serialPrint("%d: BT", n);
    }
#endif
  }
  serialCrlf();

  serialPrint("Tasks switch log at %u [<time>, <task_id>]:", get_tmr10ms());
  uint32_t lastSwitchTime = 0;
  uint32_t * tsl = new uint32_t[DEBUG_TASKS_LOG_SIZE];
  memcpy(tsl, taskSwitchLog, sizeof(taskSwitchLog));
  uint32_t * p = tsl + taskSwitchLogPos;
  uint32_t * end = tsl + DEBUG_TASKS_LOG_SIZE;
  for(int n = 0; n < DEBUG_TASKS_LOG_SIZE; n++) {
    uint32_t taskId = *p >> 24;
    uint32_t switchTime = *p & 0xFFFFFF;
    if (lastSwitchTime != switchTime) {
      serialPrintf("\r\n%06x: ", switchTime);
      lastSwitchTime = switchTime;
    }
    serialPrintf("%u ", taskId);
    if ( ++p >= end ) {
      p = tsl;
    }
  }
  delete[] tsl;
  serialCrlf();
}
#endif // #if defined(DEBUG_TASKS)

#if defined(DEBUG_TIMERS)

void printDebugTime(uint32_t time)
{
  if (time >= 30000) {
    serialPrintf("%dms", time/1000);
  }
  else {
    serialPrintf("%d.%03dms", time/1000, time%1000);
  }
}

void printDebugTimer(const char * name, DebugTimer & timer)
{
  serialPrintf("%s: ", name);
  printDebugTime( timer.getMin());
  serialPrintf(" - ");
  printDebugTime(timer.getMax());
  serialCrlf();
  timer.reset();
}
void printDebugTimers()
{
  for(int n = 0; n < DEBUG_TIMERS_COUNT; n++) {
    printDebugTimer(debugTimerNames[n], debugTimers[n]);
  }
}
#endif

#include "OsMutex.h"
extern OS_MutexID audioMutex;

void printAudioVars()
{
  for(int n = 0; n < AUDIO_BUFFER_COUNT; n++) {
    serialPrint("Audio Buffer %d: size: %u, state: %u, ", n, (uint32_t)audioBuffers[n].size, (uint32_t)audioBuffers[n].state);
    dump((uint8_t *)audioBuffers[n].data, 32);
  }
  serialPrint("fragments:");
  for(int n = 0; n < AUDIO_QUEUE_LENGTH; n++) {
    serialPrint("%d: type %u: id: %u, repeat: %u, ", n, (uint32_t)audioQueue.fragments[n].type,
                                                        (uint32_t)audioQueue.fragments[n].id,
                                                        (uint32_t)audioQueue.fragments[n].repeat);
    if ( audioQueue.fragments[n].type == FRAGMENT_FILE) {
      serialPrint(" file: %s", audioQueue.fragments[n].file);
    }
  }

  serialPrint("audioQueue:");
  serialPrint("  ridx: %d, widx: %d", audioQueue.ridx, audioQueue.widx);
  serialPrint("  bufferRIdx: %d, bufferWIdx: %d", audioQueue.bufferRIdx, audioQueue.bufferWIdx);

  serialPrint("normalContext: %u", (uint32_t)audioQueue.normalContext.fragment.type);

  serialPrint("audioMutex[%u] = %u", (uint32_t)audioMutex, (uint32_t)MutexTbl[audioMutex].mutexFlag);
}


int cliDisplay(const char ** argv)
{
  long long int address = 0;

  for (const MemArea * area = memAreas; area->name != NULL; area++) {
    if (!strcmp(area->name, argv[1])) {
      dump((uint8_t *)area->start, area->size);
      return 0;
    }
  }

  if (!strcmp(argv[1], "keys")) {
    for (int i=0; i<TRM_BASE; i++) {
      char name[8];
      uint8_t len = STR_VKEYS[0];
      strncpy(name, STR_VKEYS+1+len*i, len);
      name[len] = '\0';
      serialPrint("[%s] = %s", name, switchState(EnumKeys(i)) ? "on" : "off");
    }
#if defined(ROTARY_ENCODER_NAVIGATION) || defined(REV9E) || defined(PCBHORUS) || defined(PCBFLAMENCO)
    serialPrint("[Enc.] = %d", rotencValue / 2);
#endif
    for (int i=TRM_BASE; i<=TRM_LAST; i++) {
      serialPrint("[Trim%d] = %s", i-TRM_BASE, switchState(EnumKeys(i)) ? "on" : "off");
    }
    for (int i=MIXSRC_FIRST_SWITCH; i<=MIXSRC_LAST_SWITCH; i++) {
      mixsrc_t sw = i - MIXSRC_FIRST_SWITCH;
      if (SWITCH_EXISTS(sw)) {
        serialPrint("[S%c] = %s", 'A'+sw, (switchState((EnumKeys)(SW_BASE+(3*sw))) ? "down" : (switchState((EnumKeys)(SW_BASE+(3*sw)+1)) ? "mid" : "up")));
      }
    }
  }
  else if (!strcmp(argv[1], "adc")) {
    for (int i=0; i<NUMBER_ANALOG; i++) {
      serialPrint("adc[%d] = %04X", i, (int)adcValues[i]);
    }
  }
  else if (!strcmp(argv[1], "outputs")) {
    for (int i=0; i<NUM_CHNOUT; i++) {
      serialPrint("outputs[%d] = %04d", i, (int)channelOutputs[i]);
    }
  }
  else if (!strcmp(argv[1], "rtc")) {
    struct gtm utm;
    gettime(&utm);
    serialPrint("rtc = %4d-%02d-%02d %02d:%02d:%02d.%02d0", utm.tm_year+1900, utm.tm_mon+1, utm.tm_mday, utm.tm_hour, utm.tm_min, utm.tm_sec, g_ms100);
  }
  else if (!strcmp(argv[1], "volume")) {
    serialPrint("volume = %d", getVolume());
  }
#if defined(CPUSTM32)
  else if (!strcmp(argv[1], "uid")) {
    char str[LEN_CPU_UID+1];
    getCPUUniqueID(str);
    serialPrint("uid = %s", str);
  }
#endif
#if defined(PCBFLAMENCO)
  else if (!strcmp(argv[1], "bq24195")) {
    {
      uint8_t reg = i2cReadBQ24195(0x00);
      serialPrint(reg & 0x80 ? "HIZ enable" : "HIZ disable");
    }
    {
      uint8_t reg = i2cReadBQ24195(0x08);
      serialPrint(reg & 0x01 ? "VBatt < VSysMin" : "VBatt > VSysMin");
      serialPrint(reg & 0x02 ? "Thermal sensor bad" : "Thermal sensor ok");
      serialPrint(reg & 0x04 ? "Power ok" : "Power bad");
      serialPrint(reg & 0x08 ? "Connected to charger" : "Not connected to charger");
      static const char * const CHARGE_STATUS[] = { "Not Charging", "Precharge", "Fast Charging", "Charge done" };
      serialPrint(CHARGE_STATUS[(reg & 0x30) >> 4]);
      static const char * const INPUT_STATUS[] = { "Unknown input", "USB host input", "USB adapter port input", "OTG input" };
      serialPrint(INPUT_STATUS[(reg & 0xC0) >> 6]);
    }
    {
      uint8_t reg = i2cReadBQ24195(0x09);
      if (reg & 0x80) serialPrint("Watchdog timer expiration");
      uint8_t chargerFault = (reg & 0x30) >> 4;
      if (chargerFault == 0x01)
        serialPrint("Input fault");
      else if (chargerFault == 0x02)
        serialPrint("Thermal shutdown");
      else if (chargerFault == 0x03)
        serialPrint("Charge safety timer expiration");
      if (reg & 0x08) serialPrint("Battery over voltage fault");
      uint8_t ntcFault = (reg & 0x07);
      if (ntcFault == 0x05)
        serialPrint("NTC cold");
      else if (ntcFault == 0x06)
        serialPrint("NTC hot");
    }
  }
#endif
  else if (!strcmp(argv[1], "tim")) {
    int timerNumber;
    if (toInt(argv, 2, &timerNumber) > 0) {
      TIM_TypeDef * tim = TIM1;
      switch (timerNumber) {
        case 1:
          tim = TIM1;
          break;
        case 2:
          tim = TIM2;
          break;
        case 13:
          tim = TIM13;
          break;
        default:
          return 0;
      }
      serialPrint("TIM%d", timerNumber);
      serialPrint(" CR1    0x%x", tim->CR1);
      serialPrint(" CR2    0x%x", tim->CR2);
      serialPrint(" DIER   0x%x", tim->DIER);
      serialPrint(" SR     0x%x", tim->SR);
      serialPrint(" EGR    0x%x", tim->EGR);
      serialPrint(" CCMR1  0x%x", tim->CCMR1);
      serialPrint(" CCMR2  0x%x", tim->CCMR2);

      serialPrint(" CNT    0x%x", tim->CNT);
      serialPrint(" ARR    0x%x", tim->ARR);
      serialPrint(" PSC    0x%x", tim->PSC);

      serialPrint(" CCER   0x%x", tim->CCER);
      serialPrint(" CCR1   0x%x", tim->CCR1);
      serialPrint(" CCR2   0x%x", tim->CCR2);
      serialPrint(" CCR3   0x%x", tim->CCR3);
      serialPrint(" CCR4   0x%x", tim->CCR4);
    }
  }
  else if (!strcmp(argv[1], "dma")) {
    serialPrint("DMA1_Stream7");
    serialPrint(" CR    0x%x", DMA1_Stream7->CR);
  }
#if defined(DEBUG_INTERRUPTS)
  else if (!strcmp(argv[1], "int")) {
    printInterrupts();
  }
#endif
#if defined(DEBUG_TASKS)
  else if (!strcmp(argv[1], "tsl")) {
    printTaskSwitchLog();
  }
#endif
#if defined(DEBUG_TIMERS)
  else if (!strcmp(argv[1], "dt")) {
    printDebugTimers();
  }
#endif
  else if (!strcmp(argv[1], "audio")) {
    printAudioVars();
  }
#if defined(DISK_CACHE)
  else if (!strcmp(argv[1], "dc")) {
    DiskCacheStats stats = diskCache.getStats();
    uint32_t hitRate = diskCache.getHitRate();
    serialPrint("Disk Cache stats: reads: %u, hits: %u, hit rate: %0.1f%%", (stats.noHits + stats.noMisses), stats.noHits, hitRate/10.0);
  }
#endif
  else if (toLongLongInt(argv, 1, &address) > 0) {
    int size = 256;
    if (toInt(argv, 2, &size) >= 0) {
      dump((uint8_t *)address, size);
    }
  }
  return 0;
}

int cliDebugVars(const char ** argv)
{
#if defined(PCBHORUS)
  extern uint32_t ioMutexReq, ioMutexRel;
  extern uint32_t sdReadRetries;
  serialPrint("ioMutexReq=%d", ioMutexReq);
  serialPrint("ioMutexRel=%d", ioMutexRel);
  serialPrint("sdReadRetries=%d", sdReadRetries);
#elif defined(PCBTARANIS)
  serialPrint("telemetryErrors=%d", telemetryErrors);
#endif

  return 0;
}

int cliRepeat(const char ** argv)
{
  int interval = 0;
  int counter = 0;
  if (toInt(argv, 1, &interval) > 0 && argv[2]) {
    interval *= 50;
    counter = interval;
    uint8_t c;
    while (!cliRxFifo.pop(c) || !(c == '\r' || c == '\n' || c == ' ')) {
      CoTickDelay(10); // 20ms
      if (++counter >= interval) {
        cliExecCommand(&argv[2]);
        counter = 0;
      }
    }
  }
  else {
    serialPrint("%s: Invalid arguments", argv[0]);
  }
  return 0;
}

#if defined(JITTER_MEASURE)
int cliShowJitter(const char ** argv)
{
  serialPrint(  "#   anaIn   rawJ   avgJ");
  for (int i=0; i<NUMBER_ANALOG; i++) {
    serialPrint("A%02d %04X %04X %3d %3d", i, getAnalogValue(i), anaIn(i), rawJitter[i].get(), avgJitter[i].get());
    if (IS_POT_MULTIPOS(i)) {
      StepsCalibData * calib = (StepsCalibData *) &g_eeGeneral.calib[i];
      for (int j=0; j<calib->count; j++) {
        serialPrint("    s%d %04X", j, calib->steps[j]);
      }
    }
  }
  return 0;
}
#endif


const CliCommand cliCommands[] = {
  { "beep", cliBeep, "[<frequency>] [<duration>]" },
  { "ls", cliLs, "<directory>" },
  { "read", cliRead, "<filename>" },
  { "readsd", cliReadSD, "<start sector> <sectors count> <read buffer size (sectors)>" },
  { "play", cliPlay, "<filename>" },
  { "print", cliDisplay, "<address> [<size>] | <what>" },
  { "p", cliDisplay, "<address> [<size>] | <what>" },
  { "reboot", cliReboot, "[wdt]" },
  { "set", cliSet, "<what> <value>" },
  { "stackinfo", cliStackInfo, "" },
  { "meminfo", cliMemoryInfo, "" },
  { "trace", cliTrace, "on | off" },
#if defined(PCBFLAMENCO)
  { "read_bq24195", cliReadBQ24195, "<register>" },
  { "write_bq24195", cliWriteBQ24195, "<register> <data>" },
#endif
  { "help", cliHelp, "[<command>]" },
  { "debugvars", cliDebugVars, "" },
  { "repeat", cliRepeat, "<interval> <command>" },
#if defined(JITTER_MEASURE)
  { "jitter", cliShowJitter, "" },
#endif
  { NULL, NULL, NULL }  /* sentinel */
};

int cliHelp(const char ** argv)
{
  for (const CliCommand * command = cliCommands; command->name != NULL; command++) {
    if (argv[1][0] == '\0' || !strcmp(command->name, argv[0])) {
      serialPrint("%s %s", command->name, command->args);
      if (argv[1][0] != '\0') {
        return 0;
      }
    }
  }
  if (argv[1][0] != '\0') {
    serialPrint("Invalid command \"%s\"", argv[0]);
  }
  return -1;
}

int cliExecCommand(const char ** argv)
{
  if (argv[0][0] == '\0')
    return 0;

  for (const CliCommand * command = cliCommands; command->name != NULL; command++) {
    if (!strcmp(command->name, argv[0])) {
      return command->func(argv);
    }
  }
  serialPrint("Invalid command \"%s\"", argv[0]);
  return -1;
}

int cliExecLine(char * line)
{
  int len = strlen(line);
  const char * argv[CLI_COMMAND_MAX_ARGS];
  memset(argv, 0, sizeof(argv));
  int argc = 1;
  argv[0] = line;
  for (int i=0; i<len; i++) {
    if (line[i] == ' ') {
      line[i] = '\0';
      if (argc < CLI_COMMAND_MAX_ARGS) {
        argv[argc++] = &line[i+1];
      }
    }
  }
  return cliExecCommand(argv);
}

void cliTask(void * pdata)
{
  char line[CLI_COMMAND_MAX_LEN+1];
  int pos = 0;

  cliPrompt();

  for (;;) {
    uint8_t c;

    while (!cliRxFifo.pop(c)) {
      CoTickDelay(10); // 20ms
    }

    if (c == 12) {
      // clear screen
      serialPrint("\033[2J\033[1;1H");
      cliPrompt();
    }
    else if (c == 127) {
      // backspace
      if (pos) {
        line[--pos] = '\0';
        serialPutc(c);
      }
    }
    else if (c == '\r' || c == '\n') {
      // enter
      serialCrlf();
      line[pos] = '\0';
      // strcpy(cliLastLine, line);
      cliExecLine(line);
      pos = 0;
      cliPrompt();
    }
    else if (isascii(c) && pos < CLI_COMMAND_MAX_LEN) {
      line[pos++] = c;
      serialPutc(c);
    }
  }
}

void cliStart()
{
  cliTaskId = CoCreateTaskEx(cliTask, NULL, 10, &cliStack.stack[CLI_STACK_SIZE-1], CLI_STACK_SIZE, 1, false);
}
