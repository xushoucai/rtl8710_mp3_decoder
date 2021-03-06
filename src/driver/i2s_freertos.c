/******************************************************************************
 * Copyright 2013-2015 Espressif Systems
 *
 * FileName: i2s_freertos.c
 *
 * Description: I2S output routines for a FreeRTOS system. Uses DMA and a queue
 * to abstract away the nitty-gritty details.
 *
 * Modification history:
 *     2015/06/01, v1.0 File created.
*******************************************************************************/

/*
How does this work? Basically, to get sound, you need to:
- Connect an I2S codec to the I2S pins on the RTL.
- Start up a thread that's going to do the sound output
- Call I2sInit()
- Call I2sSetRate() with the sample rate you want.
- Generate sound and call i2sPushSample() with 32-bit samples.
The 32bit samples basically are 2 16-bit signed values (the analog values for
the left and right channel) concatenated as (Rout<<16)+Lout

I2sPushSample will block when you're sending data too quickly, so you can just
generate and push data as fast as you can and I2sPushSample will regulate the
speed.
*/


#include "rtl_common.h"

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "queue.h"

//RTL's
#include "i2s_api.h"
#include "i2s_freertos.h"

i2s_t i2s_obj;
u8 i2s_tx_buf[I2S_DMA_PAGE_SIZE*I2S_DMA_PAGE_NUM];
u8 i2s_rx_buf[I2S_DMA_PAGE_SIZE*I2S_DMA_PAGE_NUM];

#include <section_config.h>

//ESP's
//Queue which contains empty DMA buffers
static xQueueHandle dmaQueue;
//DMA underrun counter
static long underrunCnt;

//This routine is called as soon as the DMA routine has something to tell us. 

//RTL's interrupt callback
void test_tx_complete(void *data, char *pbuf)
{
	portBASE_TYPE HPTaskAwoken=0;
    int *ptx_buf;

    i2s_t *obj = (i2s_t *)data;
    static u32 count=0;
    //DBG_8195A_I2S_LVL(VERI_I2S_LVL, "I2S%d %s\n",pI2SDemoHnd->DevNum,__func__);
    count++;
    if ((count&1023) == 1023)
    {
         DBG_8195A_I2S_LVL(VERI_I2S_LVL, ",\n");
    }

    ptx_buf = i2s_get_tx_page(obj);
    ptx_buf = i2s_get_tx_page(obj);
	if (ptx_buf) {
		xQueueSendFromISR(dmaQueue, ptx_buf, &HPTaskAwoken);
	}
	portEND_SWITCHING_ISR(HPTaskAwoken);
}

void test_rx_complete(void *data, char* pbuf)
{
    i2s_t *obj = (i2s_t *)data;
    int *ptx_buf;

    static u32 count=0;
    count++;
    if ((count&1023) == 1023)
    {
         DBG_8195A_I2S_LVL(VERI_I2S_LVL, ".\n");
    }

    ptx_buf = i2s_get_tx_page(obj);
	if (ptx_buf) {
		xQueueSendFromISR(dmaQueue, ptx_buf, ( TickType_t ) 1000);
	}
}

//Initialize I2S subsystem for DMA circular buffer use
void ICACHE_FLASH_ATTR i2sInit() {
	//RTL's I2S init
    int *ptx_buf;
    int i,j;

	i2s_obj.channel_num = CH_STEREO;
	i2s_obj.sampling_rate = SR_44p1KHZ;
	i2s_obj.word_length = WL_16b;
	i2s_obj.direction = I2S_DIR_TXRX; //consider switching to TX only  
	i2s_init(&i2s_obj, I2S_SCLK_PIN, I2S_WS_PIN, I2S_SD_PIN);
    i2s_set_dma_buffer(&i2s_obj, (char*)i2s_tx_buf, (char*)i2s_rx_buf, \
        I2S_DMA_PAGE_NUM, I2S_DMA_PAGE_SIZE);
    i2s_tx_irq_handler(&i2s_obj, (i2s_irq_handler)test_tx_complete, (uint32_t)&i2s_obj);
    i2s_rx_irq_handler(&i2s_obj, (i2s_irq_handler)test_rx_complete, (uint32_t)&i2s_obj);

	//We use a queue to keep track of the DMA buffers that are empty. The ISR will push buffers to the back of the queue,
	//the mp3 decode will pull them from the front and fill them. For ease, the queue will contain *pointers* to the DMA
	//buffers, not the data itself. The queue depth is one smaller than the amount of buffers we have, because there's
	//always a buffer that is being used by the DMA subsystem *right now* and we don't want to be able to write to that
	//simultaneously.
	dmaQueue=xQueueCreate(I2S_DMA_PAGE_NUM-1, sizeof(int*));

	underrunCnt=0;
	
	i2s_set_param(&i2s_obj, i2s_obj.channel_num, i2s_obj.sampling_rate, WL_16b);
	DBG_8195A("I2S Init\n");
    for (i=0;i<I2S_DMA_PAGE_NUM;i++) {
        ptx_buf = i2s_get_tx_page(&i2s_obj);
        if (ptx_buf) {
			xQueueSend(dmaQueue, ptx_buf, portMAX_DELAY);
        }
    }
}


//#define BASEFREQ (160000000L)
#define ABS(x) (((x)>0)?(x):(-(x)))

//Set the I2S sample rate, in HZ
void ICACHE_FLASH_ATTR i2sSetRate(int rate, int lockBitcount) {		
	//(lockBitcount?17:20) - 16+1 or 19+1 bits
	int sample_rate = SR_96KHZ;
	if (rate<=96000 || ABS(rate-96000)<ABS(rate-88200)) sample_rate = SR_96KHZ;
    else if (rate<=88200 || ABS(rate-88200)<ABS(rate-48000)) sample_rate = SR_88p2KHZ;
	else if (rate<=48000 || ABS(rate-48000)<ABS(rate-44100)) sample_rate = SR_48KHZ;
    else if (rate<=44100 || ABS(rate-44100)<ABS(rate-32000)) sample_rate = SR_44p1KHZ;
	else if (rate<=32000 || ABS(rate-32000)<ABS(rate-24000)) sample_rate = SR_32KHZ;
	else if (rate<=24000 || ABS(rate-24000)<ABS(rate-22050)) sample_rate = SR_24KHZ;
	else if (rate<=22050 || ABS(rate-22050)<ABS(rate-16000)) sample_rate = SR_22p05KHZ;
	else if (rate<=16000 || ABS(rate-16000)<ABS(rate-11020)) sample_rate = SR_16KHZ;
	else if (rate<=11020 || ABS(rate-11020)<ABS(rate- 8000)) sample_rate = SR_11p02KHZ;
	else if (rate<= 8000 || ABS(rate- 8000)<ABS(rate- 7350)) sample_rate = SR_8KHZ;
	else sample_rate = SR_7p35KHZ;
	
	i2s_obj.sampling_rate = sample_rate;
    	
	i2s_set_param(&i2s_obj, i2s_obj.channel_num, i2s_obj.sampling_rate, WL_16b);

	DBG_8195A("ReqRate %d Sample Rate %d\n", rate, sample_rate);
	
	// ESP's
	//Find closest divider 

}

//Current DMA buffer we're writing to
static unsigned int *currDMABuff=NULL;
//Current position in that DMA buffer
static int currDMABuffPos=0;


//This routine pushes a single, 32-bit sample to the I2S buffers. Call this at (on average) 
//at least the current sample rate. You can also call it quicker: it will suspend the calling
//thread if the buffer is full and resume when there's room again.
void i2sPushSample(unsigned int sample) {
	if (currDMABuff==NULL) {
		//We need a new buffer. Pop one from the queue.
		xQueueReceive(dmaQueue, &currDMABuff, portMAX_DELAY);
		currDMABuffPos=0;
	}
	
	currDMABuff[currDMABuffPos++]=sample;
	
	//Check if current DMA buffer is full.
	if (currDMABuffPos==I2S_DMA_PAGE_SIZE) {
		i2s_send_page(&i2s_obj, &currDMABuff);
		xQueueReceive(dmaQueue, &currDMABuff, portMAX_DELAY);
		currDMABuffPos=0;
	}	
}


long ICACHE_FLASH_ATTR i2sGetUnderrunCnt() {
	return underrunCnt;
}
