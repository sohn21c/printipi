#include "dmascheduler.h"

#include <sys/mman.h> //for mmap
#include <sys/time.h> //for timespec
#include <time.h> //for timespec / nanosleep / usleep (need -std=gnu99)
#include <signal.h> //for sigaction
#include <unistd.h> //for NULL
//#include <stdio.h> //for printf
#include <stdlib.h> //for exit
#include <fcntl.h> //for file opening
#include <errno.h> //for errno
#include <pthread.h> //for pthread_setschedparam
#include <chrono>

#include "schedulerbase.h"
#include "common/logging.h"
#include "common/typesettings/clocks.h"


namespace drv {
namespace rpi {

//initialize static variables:
DmaChannelHeader *DmaScheduler::dmaHeader(0);

void writeBitmasked(volatile uint32_t *dest, uint32_t mask, uint32_t value) {
    //set bits designated by (mask) at the address (dest) to (value), without affecting the other bits
    //eg if x = 0b11001100
    //  writeBitmasked(&x, 0b00000110, 0b11110011),
    //  then x now = 0b11001110
    uint32_t cur = *dest;
    uint32_t revised = (cur & (~mask)) | (value & mask);
    *dest = revised;
    *dest = revised; //best to be safe when crossing memory boundaries
}

size_t ceilToPage(size_t size) {
    //round up to nearest page-size multiple
    if (size & (PAGE_SIZE-1)) {
        size += PAGE_SIZE - (size & (PAGE_SIZE-1));
    }
    return size;
}

//allocate some memory and lock it so that its physical address will never change
uint8_t* makeLockedMem(size_t size) {
    size = ceilToPage(size);
    void *mem = mmap(
        NULL,   //let kernel place memory where it wants
        size,   //length
        PROT_WRITE | PROT_READ, //ask for read and write permissions to memory
        MAP_SHARED | 
        MAP_ANONYMOUS | //no underlying file; initialize to 0
        MAP_NORESERVE | //don't reserve swap space
        MAP_LOCKED, //lock into *virtual* ram. Physical ram may still change!
        -1,	// File descriptor
    0); //no offset into file (file doesn't exist).
    if (mem == MAP_FAILED) {
        LOGE("dmascheduler.cpp: makeLockedMem failed\n");
        exit(1);
    }
    memset(mem, 0, size); //simultaneously zero the pages and force them into memory
    mlock(mem, size);
    return (uint8_t*)mem;
}

//free memory allocated with makeLockedMem
void freeLockedMem(void* mem, size_t size) {
    size = ceilToPage(size);
    munlock(mem, size);
    munmap(mem, size);
}


DmaScheduler::DmaScheduler() {
    dmaCh = 5;
    SchedulerBase::registerExitHandler(&cleanup, SCHED_IO_EXIT_LEVEL);
    makeMaps();
    initSrcAndControlBlocks();
    initPwm();
    initDma();
}

void DmaScheduler::cleanup() {
    LOG("DmaScheduler::cleanup\n");
    //disable DMA. Otherwise, it will continue to run in the background, potentially overwriting future user data.
    if(dmaHeader) {
        writeBitmasked(&dmaHeader->CS, DMA_CS_ACTIVE, 0);
        usleep(100);
        writeBitmasked(&dmaHeader->CS, DMA_CS_RESET, DMA_CS_RESET);
    }
    //could also disable PWM, but that's not imperative.
}

void DmaScheduler::makeMaps() {
    memfd = open("/dev/mem", O_RDWR | O_SYNC);
    if (memfd < 0) {
        LOGE("Failed to open /dev/mem (did you remember to run as root?)\n");
        exit(1);
    }
    pagemapfd = open("/proc/self/pagemap", O_RDONLY);
    //now map /dev/mem into memory, but only map specific peripheral sections:
    gpioBaseMem = mapPeripheral(GPIO_BASE);
    dmaBaseMem = mapPeripheral(DMA_BASE);
    pwmBaseMem = mapPeripheral(PWM_BASE);
    timerBaseMem = mapPeripheral(TIMER_BASE);
    clockBaseMem = mapPeripheral(CLOCK_BASE);
}

volatile uint32_t* DmaScheduler::mapPeripheral(int addr) const {
    ///dev/mem behaves as a file. We need to map that file into memory:
    //NULL = virtual address of mapping is chosen by kernel.
    //PAGE_SIZE = map 1 page.
    //PROT_READ|PROT_WRITE means give us read and write priveliges to the memory
    //MAP_SHARED means updates to the mapped memory should be written back to the file & shared with other processes
    //addr = offset in file to map
    void *mapped = mmap(NULL, PAGE_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, memfd, addr);
    //now, *mapped = memory at physical address of addr.
    if (mapped == MAP_FAILED) {
        LOGE("DmaScheduler::mapPeripheral failed to map memory (did you remember to run as root?)\n");
        exit(1);
    } else {
        LOGV("DmaScheduler::mapPeripheral mapped: %p\n", mapped);
    }
    return (volatile uint32_t*)mapped;
}

void DmaScheduler::initSrcAndControlBlocks() {
    //Often need to copy zeros with DMA. This array can be the source. Needs to all lie on one page
    zerosPageCached = makeLockedMem(PAGE_SIZE);
    zerosPage = makeUncachedMemView(zerosPageCached, PAGE_SIZE);
    
    //configure DMA...
    //First, allocate memory for the source:
    size_t numSrcBlocks = SOURCE_BUFFER_FRAMES; //We want apx 1M blocks/sec.
    size_t srcPageBytes = numSrcBlocks*sizeof(struct GpioBufferFrame);
    virtSrcPageCached = makeLockedMem(srcPageBytes);
    virtSrcPage = makeUncachedMemView(virtSrcPageCached, srcPageBytes);
    LOGV("DmaScheduler::initSrcAndControlBlocks mappedPhysSrcPage: %08x\n", virtToPhys(virtSrcPage));
    
    //cast virtSrcPage to a GpioBufferFrame array:
    srcArray = (struct GpioBufferFrame*)virtSrcPage; //Note: calling virtToPhys on srcArray will return NULL. Use srcArrayCached for that.
    srcArrayCached = (struct GpioBufferFrame*)virtSrcPageCached;
    
    //allocate memory for the control blocks
    size_t cbPageBytes = numSrcBlocks * sizeof(struct DmaControlBlock) * 3; //3 cbs for each source block
    virtCbPageCached = makeLockedMem(cbPageBytes);
    virtCbPage = makeUncachedMemView(virtCbPageCached, cbPageBytes);
    //fill the control blocks:
    cbArrCached = (struct DmaControlBlock*)virtCbPageCached;
    cbArr = (struct DmaControlBlock*)virtCbPage;
    LOG("DmaScheduler::initSrcAndControlBlocks: #dma blocks: %i, #src blocks: %i\n", numSrcBlocks*3, numSrcBlocks);
    for (unsigned int i=0; i<numSrcBlocks*3; i += 3) {
        //pace DMA through PWM
        cbArr[i].TI = DMA_CB_TI_PERMAP_PWM | DMA_CB_TI_DEST_DREQ | DMA_CB_TI_NO_WIDE_BURSTS | DMA_CB_TI_TDMODE;
        cbArr[i].SOURCE_AD = virtToUncachedPhys(srcArrayCached + i/3); //The data written doesn't matter, but using the GPIO source will hopefully bring it into L2 for more deterministic timing of the next control block.
        cbArr[i].DEST_AD = PWM_BASE_BUS + PWM_FIF1; //write to the FIFO
        cbArr[i].TXFR_LEN = DMA_CB_TXFR_LEN_YLENGTH(1) | DMA_CB_TXFR_LEN_XLENGTH(4);
        cbArr[i].STRIDE = i/3;
        cbArr[i].NEXTCONBK = virtToUncachedPhys(cbArrCached+i+1); //have to use the cached version because the uncached version isn't listed in pagemap(?)
        //copy buffer to GPIOs
        cbArr[i+1].TI = DMA_CB_TI_SRC_INC | DMA_CB_TI_DEST_INC | DMA_CB_TI_NO_WIDE_BURSTS | DMA_CB_TI_TDMODE;
        cbArr[i+1].SOURCE_AD = virtToUncachedPhys(srcArrayCached + i/3);
        cbArr[i+1].DEST_AD = GPIO_BASE_BUS + GPSET0;
        cbArr[i+1].TXFR_LEN = DMA_CB_TXFR_LEN_YLENGTH(2) | DMA_CB_TXFR_LEN_XLENGTH(8);
        cbArr[i+1].STRIDE = DMA_CB_STRIDE_D_STRIDE(4) | DMA_CB_STRIDE_S_STRIDE(0);
        cbArr[i+1].NEXTCONBK = virtToUncachedPhys(cbArrCached+i+2);
        //clear buffer (TODO: investigate using a 4-word copy ("burst") )
        cbArr[i+2].TI = DMA_CB_TI_DEST_INC | DMA_CB_TI_NO_WIDE_BURSTS | DMA_CB_TI_TDMODE;
        cbArr[i+2].SOURCE_AD = virtToUncachedPhys(zerosPageCached);
        cbArr[i+2].DEST_AD = virtToUncachedPhys(srcArrayCached + i/3);
        cbArr[i+2].TXFR_LEN = DMA_CB_TXFR_LEN_YLENGTH(1) | DMA_CB_TXFR_LEN_XLENGTH(sizeof(struct GpioBufferFrame));
        cbArr[i+2].STRIDE = i/3; //might be better to use the NEXT index
        int nextIdx = i+3 < numSrcBlocks*3 ? i+3 : 0; //last block should loop back to the first block
        cbArr[i+2].NEXTCONBK = virtToUncachedPhys(cbArrCached + nextIdx); //(uint32_t)physCbPage + ((void*)&cbArr[(i+2)%maxIdx] - virtCbPage);
    }
}

uint8_t* DmaScheduler::makeUncachedMemView(void* virtaddr, size_t bytes) const {
    //by default, writing to any virtual address will go through the CPU cache.
    //this function will return a pointer that behaves the same as virtaddr, but bypasses the CPU L1 cache (note that because of this, the returned pointer and original pointer should not be used in conjunction, else cache-related inconsistencies will arise)
    //Note: The original memory should not be unmapped during the lifetime of the uncached version, as then the OS won't know that our process still owns the physical memory.
    bytes = ceilToPage(bytes);
    //first, just allocate enough *virtual* memory for the operation. This is done so that we can do the later mapping to a contiguous range of virtual memory:
    void *mem = mmap(
        NULL,   //let kernel place memory where it wants
        bytes,   //length
        PROT_WRITE | PROT_READ, //ask for read and write permissions to memory
        MAP_SHARED | 
        MAP_ANONYMOUS | //no underlying file; initialize to 0
        MAP_NORESERVE | //don't reserve swap space
        MAP_LOCKED, //lock into *virtual* ram. Physical ram may still change!
        -1,	// File descriptor
    0); //no offset into file (file doesn't exist).
    uint8_t *memBytes = (uint8_t*)mem;
    //now, free the virtual memory and immediately remap it to the physical addresses used in virtaddr
    munmap(mem, bytes); //Might not be necessary; MAP_FIXED indicates it can map an already-used page
    for (unsigned int offset=0; offset<bytes; offset += PAGE_SIZE) {
        void *mappedPage = mmap(memBytes+offset, PAGE_SIZE, PROT_WRITE|PROT_READ, MAP_SHARED|MAP_FIXED|MAP_NORESERVE|MAP_LOCKED, memfd, virtToUncachedPhys((uint8_t*)virtaddr+offset));
        if (mappedPage != memBytes+offset) { //We need these mappings to be contiguous over virtual memory (in order to replicate the virtaddr array), so we must ensure that the address we requested from mmap was actually used.
            LOGE("DmaScheduler::makeUncachedMemView: failed to create an uncached view of memory at addr %p+0x%08x\n", virtaddr, offset);
            exit(1);
        }
    }
    memset(mem, 0, bytes); //Although the cached version might have been reset, those writes might not have made it through.
    return memBytes;
}

uintptr_t DmaScheduler::virtToPhys(void* virt) const {
    //uintptr_t pgNum = (uintptr_t)(virt)/PAGE_SIZE;
    int pgNum = (uintptr_t)(virt)/PAGE_SIZE;
    int byteOffsetFromPage = (uintptr_t)(virt)%PAGE_SIZE;
    uint64_t physPage;
    ///proc/self/pagemap is a uint64_t array where the index represents the virtual page number and the value at that index represents the physical page number.
    //So if virtual address is 0x1000000, read the value at *array* index 0x1000000/PAGE_SIZE and multiply that by PAGE_SIZE to get the physical address.
    //because files are bytestreams, one must explicitly multiply each byte index by 8 to treat it as a uint64_t array.
    int err = lseek(pagemapfd, pgNum*8, SEEK_SET);
    if (err != pgNum*8) {
        LOGW("WARNING: DmaScheduler::virtToPhys %p failed to seek (expected %i got %i. errno: %i)\n", virt, pgNum*8, err, errno);
    }
    read(pagemapfd, &physPage, 8);
    if (!physPage & (1ull<<63)) { //bit 63 is set to 1 if the page is present in ram
        LOGW("WARNING: DmaScheduler::virtToPhys %p has no physical address\n", virt);
    }
    physPage = physPage & ~(0x1ffull << 55); //bits 55-63 are flags.
    uintptr_t mapped = (uintptr_t)(physPage*PAGE_SIZE + byteOffsetFromPage);
    return mapped;
}
uintptr_t DmaScheduler::virtToUncachedPhys(void *virt) const {
    return virtToPhys(virt) | 0x40000000; //bus address of the ram is 0x40000000. With this binary-or, writes to the returned address will bypass the CPU (L1) cache, but not the L2 cache. 0xc0000000 should be the base address if L2 must also be bypassed. However, the DMA engine is aware of L2 cache - just not the L1 cache (source: http://en.wikibooks.org/wiki/Aros/Platforms/Arm_Raspberry_Pi_support#Framebuffer )
}

void DmaScheduler::initPwm() {
    //configure PWM clock:
    *(clockBaseMem + CM_PWMCTL/4) = CM_PWMCTL_PASSWD | ((*(clockBaseMem + CM_PWMCTL/4))&(~CM_PWMCTL_ENAB)); //disable clock
    do {} while (*(clockBaseMem + CM_PWMCTL/4) & CM_PWMCTL_BUSY); //wait for clock to deactivate
    *(clockBaseMem + CM_PWMDIV/4) = CM_PWMDIV_PASSWD | CM_PWMDIV_DIVI(50); //configure clock divider (running at 500MHz undivided)
    *(clockBaseMem + CM_PWMCTL/4) = CM_PWMCTL_PASSWD | CM_PWMCTL_SRC_PLLD; //source 500MHz base clock, no MASH.
    *(clockBaseMem + CM_PWMCTL/4) = CM_PWMCTL_PASSWD | CM_PWMCTL_SRC_PLLD | CM_PWMCTL_ENAB; //enable clock
    do {} while ((*(clockBaseMem + CM_PWMCTL/4) & CM_PWMCTL_BUSY) == 0); //wait for clock to activate
    
    //configure rest of PWM:
    struct PwmHeader *pwmHeader = (struct PwmHeader*)(pwmBaseMem);
    
    pwmHeader->DMAC = 0; //disable DMA
    pwmHeader->CTL |= PWM_CTL_CLRFIFO; //clear pwm
    usleep(100);
    
    pwmHeader->STA = PWM_STA_ERRS; //clear PWM errors
    usleep(100);
    
    pwmHeader->DMAC = PWM_DMAC_EN | PWM_DMAC_DREQ(PWM_FIFO_SIZE) | PWM_DMAC_PANIC(PWM_FIFO_SIZE); //DREQ is activated at queue < PWM_FIFO_SIZE
    pwmHeader->RNG1 = 10; //used only for timing purposes; #writes to PWM FIFO/sec = PWM CLOCK / RNG1
    pwmHeader->CTL = PWM_CTL_REPEATEMPTY1 | PWM_CTL_ENABLE1 | PWM_CTL_USEFIFO1;
}

void DmaScheduler::initDma() {
    //enable DMA channel (it's probably already enabled, but we want to be sure):
    writeBitmasked(dmaBaseMem + DMAENABLE/4, 1 << dmaCh, 1 << dmaCh);
    
    //configure the DMA header to point to our control block:
    dmaHeader = (struct DmaChannelHeader*)(dmaBaseMem + DMACH(dmaCh)/4); //must divide by 4, as dmaBaseMem is uint32_t*
    //LOGV("Previous DMA header:\n");
    //logDmaChannelHeader(dmaHeader);
    //abort any previous DMA:
    //dmaHeader->NEXTCONBK = 0; //NEXTCONBK is read-only.
    dmaHeader->CS |= DMA_CS_ABORT; //make sure to disable dma first.
    usleep(100); //give time for the abort command to be handled.
    
    dmaHeader->CS = DMA_CS_RESET;
    usleep(100);
    
    writeBitmasked(&dmaHeader->CS, DMA_CS_END, DMA_CS_END); //clear the end flag
    dmaHeader->DEBUG = DMA_DEBUG_READ_ERROR | DMA_DEBUG_FIFO_ERROR | DMA_DEBUG_READ_LAST_NOT_SET_ERROR; // clear debug error flags
    uint32_t firstAddr = virtToUncachedPhys(cbArrCached);
    LOG("DmaScheduler::initDma: starting DMA @ CONBLK_AD=0x%08x\n", firstAddr);
    dmaHeader->CONBLK_AD = firstAddr; //(uint32_t)physCbPage + ((void*)cbArr - virtCbPage); //we have to point it to the PHYSICAL address of the control block (cb1)
    dmaHeader->CS = DMA_CS_PRIORITY(7) | DMA_CS_PANIC_PRIORITY(7) | DMA_CS_DISDEBUG; //high priority (max is 7)
    dmaHeader->CS = DMA_CS_PRIORITY(7) | DMA_CS_PANIC_PRIORITY(7) | DMA_CS_DISDEBUG | DMA_CS_ACTIVE; //activate DMA. 
}

void DmaScheduler::queue(int pin, int mode, uint64_t micros) {
    //This function takes a pin, a mode (0=off, 1=on) and a time. It then manipulates the GpioBufferFrame array in order to ensure that the pin switches to the desired level at the desired time. It will sleep if necessary.
    //Sleep until we are on the right iteration of the circular buffer (otherwise we cannot queue the command)
    uint64_t callTime = std::chrono::duration_cast<std::chrono::microseconds>(EventClockT::now().time_since_epoch()).count(); //only used for debugging
    uint64_t desiredTime = micros-((uint64_t)SOURCE_BUFFER_FRAMES)*1000000/FRAMES_PER_SEC;
    //sleepUntilMicros(desiredTime);
    SleepT::sleep_until(std::chrono::time_point<std::chrono::microseconds>(std::chrono::microseconds(desiredTime)));
    uint64_t awakeTime = std::chrono::duration_cast<std::chrono::microseconds>(EventClockT::now().time_since_epoch()).count(); //only used for debugging
    
    //get the current source index at the current time:
    //must ensure we aren't interrupted during this calculation, hence the two timers instead of 1. 
    //Note: getting the curTime & srcIdx don't have to be done for every call to queue - it could be done eg just once per buffer.
    //  It should be calculated regularly though, to counter clock drift & PWM FIFO underflows
    //  It is done in this function only for simplicity
    int srcIdx;
    EventClockT::time_point curTime1, curTime2;
    int tries=0;
    do {
        //curTime1 = readSysTime();
        curTime1 = EventClockT::now();
        srcIdx = dmaHeader->STRIDE; //the source index is stored in the otherwise-unused STRIDE register, for efficiency
        //curTime2 = readSysTime();
        curTime2 = EventClockT::now();
        ++tries;
    } while (std::chrono::duration_cast<std::chrono::microseconds>(curTime2-curTime1).count() > 1 || (srcIdx & DMA_CB_TXFR_YLENGTH_MASK)); //allow 1 uS variability.
    //calculate the frame# at which to place the event:
    uint64_t usecFromNow = micros - std::chrono::duration_cast<std::chrono::microseconds>(curTime2.time_since_epoch()).count();
    int framesFromNow = usecFromNow*FRAMES_PER_SEC/1000000; //Note: may cause overflow if FRAMES_PER_SECOND is not a multiple of 1000000 or if optimizations are COMPLETELY disabled.
    if (framesFromNow < 10) { //Not safe to schedule less than ~10uS into the future (note: should be operating on usecFromNow, not framesFromNow)
        LOGW("Warning: DmaScheduler behind schedule: %i (%llu) (tries: %i) (sleep %llu -> %llu (wanted %llu for %llu now is %llu))\n", framesFromNow, usecFromNow, tries, callTime, awakeTime, desiredTime, micros, curTime2.time_since_epoch().count());
        framesFromNow = 10;
    }
    int newIdx = (srcIdx + framesFromNow)%SOURCE_BUFFER_FRAMES;
    //Now queue the command:
    if (mode == 0) { //turn output off
        srcArray[newIdx].gpclr[pin>31] |= 1 << (pin%32);
    } else { //turn output on
        srcArray[newIdx].gpset[pin>31] |= 1 << (pin%32);
    }
}

/*void DmaScheduler::sleepUntilMicros(uint64_t micros) const {
    //Note: cannot use clock_nanosleep with an absolute time, as the process clock may differ from the RPi clock.
    //this function doesn't need to be super precise, so we can tolerate interrupts.
    //Therefore, we can use a relative sleep:
    uint64_t cur = readSysTime();
    if (micros > cur) { //avoid overflow caused by unsigned arithmetic
        uint64_t dur = micros - cur;
        //usleep(dur); //nope, causes problems!
        struct timespec t;
        t.tv_sec = dur/1000000;
        t.tv_nsec = (dur - t.tv_sec*1000000)*1000;
        nanosleep(&t, NULL);
    }
}*/

}
}
