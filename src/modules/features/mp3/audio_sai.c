#include "audio_sai.h"
#include "stm32h7xx.h"   /* CMSIS register defs (SAI1_Block_A, DMA1_Stream0, RCC, GPIOE, SCB) */

/* SAI1-A kernel clock = PLL2P = 98.304 MHz (board.c). For I2S 16-bit with MCLK at 256*Fs,
 * MCKDIV = 98.304 MHz / (256 * 48000) = 8 -> exactly 48 kHz. */
#define SAI_MCKDIV 8u

static void gpio_init_sai(void) {
    /* PE4=FS_A, PE5=SCK_A, PE6=SD_A -> AF6_SAI1, push-pull, no pull. GPIOE clock is already
     * enabled by board.c (speaker enable PE3 lives on the same port). */
    const uint32_t pins[] = {4, 5, 6};
    for (unsigned i = 0; i < 3; i++) {
        uint32_t p = pins[i];
        GPIOE->MODER   = (GPIOE->MODER   & ~(0x3u << (p * 2))) | (0x2u << (p * 2)); /* AF */
        GPIOE->PUPDR   =  GPIOE->PUPDR   & ~(0x3u << (p * 2));                       /* none */
        GPIOE->OTYPER &= ~(1u << p);                                                /* PP */
        uint32_t r = p >> 3, sh = (p & 7u) * 4u;
        GPIOE->AFR[r] = (GPIOE->AFR[r] & ~(0xFu << sh)) | (6u << sh);               /* AF6 */
    }
}

void audio_clean_range(const void *addr, uint32_t bytes) {
    SCB_CleanDCache_by_Addr((uint32_t *)((uintptr_t)addr & ~31u),
                            (int32_t)(bytes + 32u));
}

void audio_start(int16_t *buf, uint32_t nsamp) {
    SAI_Block_TypeDef *A = SAI1_Block_A;

    /* Route the SAI1 kernel clock to PLL2P (98.304 MHz = 2048 x 48 kHz, the audio family).
     * board.c brings PLL2 up (for the ADC) but leaves SAI1 on its ~280 MHz default
     * (PLL1Q) -> with MCKDIV=8 that gives ~137 kHz, ~2.85x too fast. SAI1SEL = 001 = PLL2P
     * makes MCKDIV=8 land on exactly 48 kHz. */
    RCC->CDCCIP1R = (RCC->CDCCIP1R & ~RCC_CDCCIP1R_SAI1SEL_Msk) | RCC_CDCCIP1R_SAI1SEL_0;

    RCC->APB2ENR |= RCC_APB2ENR_SAI1EN;
    RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN;
    (void)RCC->APB2ENR; (void)RCC->AHB1ENR;   /* RCC enable barrier */

    gpio_init_sai();

    /* Disable the block before (re)configuring. */
    A->CR1 &= ~SAI_xCR1_SAIEN_Msk;
    while (A->CR1 & SAI_xCR1_SAIEN_Msk) { }

    /* CR1: master TX (MODE=00), free protocol (PRTCFG=00), 16-bit (DS=100), MONO,
     * master divider used (NODIV=0), master clock GENERATED (MCKEN=1), MCKDIV=8.
     * MCKEN is essential: with it the divider yields MCLK and FS = ker/(MCKDIV*256) =
     * 48 kHz; WITHOUT it the divider drives the bit clock instead, giving
     * FS = ker/(MCKDIV*32) = 384 kHz -- audio plays 8x too fast. */
    A->CR1 = (0x4u << SAI_xCR1_DS_Pos)
           | SAI_xCR1_MONO_Msk
           | SAI_xCR1_MCKEN_Msk
           | (SAI_MCKDIV << SAI_xCR1_MCKDIV_Pos);
    /* CR2: FIFO threshold = full. */
    A->CR2 = (0x4u << SAI_xCR2_FTH_Pos);
    /* FRCR: I2S Philips frame: 32-bit frame (FRL=31), FS active 16 bits (FSALL=15),
     * FSDEF=1 (FS = start-of-frame + channel), FSOFF=1 (FS one bit before MSB), FSPOL=0. */
    A->FRCR = (31u << SAI_xFRCR_FRL_Pos)
            | (15u << SAI_xFRCR_FSALL_Pos)
            | SAI_xFRCR_FSDEF_Msk
            | SAI_xFRCR_FSOFF_Msk;
    /* SLOTR: 2 slots (NBSLOT=1), 16-bit slot size (SLOTSZ=01), enable slots 0 and 1. */
    A->SLOTR = (1u << SAI_xSLOTR_NBSLOT_Pos)
             | (1u << SAI_xSLOTR_SLOTSZ_Pos)
             | (0x3u << SAI_xSLOTR_SLOTEN_Pos);

    /* DMAMUX1 channel 0 (drives DMA1_Stream0) -> SAI1_A request (87). */
    DMAMUX1_Channel0->CCR = 87u;

    /* DMA1_Stream0: circular, mem->periph, 16-bit, memory-incrementing. */
    DMA1_Stream0->CR &= ~DMA_SxCR_EN_Msk;
    while (DMA1_Stream0->CR & DMA_SxCR_EN_Msk) { }
    DMA1->LIFCR = 0x3Fu;                       /* clear stream-0 interrupt flags */
    DMA1_Stream0->PAR  = (uint32_t)(uintptr_t)&A->DR;
    DMA1_Stream0->M0AR = (uint32_t)(uintptr_t)buf;
    DMA1_Stream0->NDTR = nsamp;
    DMA1_Stream0->CR = (1u << DMA_SxCR_DIR_Pos)     /* mem -> periph */
                     | DMA_SxCR_MINC_Msk
                     | (1u << DMA_SxCR_MSIZE_Pos)   /* halfword */
                     | (1u << DMA_SxCR_PSIZE_Pos)   /* halfword */
                     | DMA_SxCR_CIRC_Msk;
    DMA1_Stream0->FCR = DMA_SxFCR_DMDIS_Msk | (0x3u << DMA_SxFCR_FTH_Pos); /* FIFO, full */
    DMA1_Stream0->CR |= DMA_SxCR_EN_Msk;

    /* Enable SAI DMA requests, then the block. */
    A->CR1 |= SAI_xCR1_DMAEN_Msk;
    A->CR1 |= SAI_xCR1_SAIEN_Msk;
}

void audio_stop(void) {
    DMA1_Stream0->CR &= ~DMA_SxCR_EN_Msk;
    while (DMA1_Stream0->CR & DMA_SxCR_EN_Msk) { }
    SAI1_Block_A->CR1 &= ~(SAI_xCR1_DMAEN_Msk | SAI_xCR1_SAIEN_Msk);
}

uint32_t audio_pos(uint32_t nsamp) {
    /* NDTR counts DOWN from nsamp; the play index is nsamp - NDTR. */
    uint32_t ndtr = DMA1_Stream0->NDTR & 0xFFFFu;
    if (ndtr == 0 || ndtr > nsamp) return 0;
    return nsamp - ndtr;
}
