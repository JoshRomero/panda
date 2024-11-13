#define SOUND_RX_BUF_SIZE 2000U
#define SOUND_TX_BUF_SIZE (SOUND_RX_BUF_SIZE/2U)
#define MIC_RX_BUF_SIZE 1000U

__attribute__((section(".sram4"))) static uint16_t sound_rx_buf[2][SOUND_RX_BUF_SIZE];
__attribute__((section(".sram4"))) static uint32_t mic_rx_buf[2][MIC_RX_BUF_SIZE];

static uint8_t sound_idle_count;

// Playback processing
static void BDMA_Channel0_IRQ_Handler(void) {
  __attribute__((section(".sram4"))) static uint16_t tx_buf[SOUND_TX_BUF_SIZE];

  BDMA->IFCR |= BDMA_IFCR_CGIF0; // clear flag

  // process samples (shift to 12b and bias to be unsigned)
  // since we are playing mono and receiving stereo, we take every other sample
  uint8_t buf_idx = (((BDMA_Channel0->CCR & BDMA_CCR_CT) >> BDMA_CCR_CT_Pos) == 1U) ? 0U : 1U;
  for (uint16_t i=0U; i < SOUND_RX_BUF_SIZE; i += 2U) {
    tx_buf[i/2U] = ((sound_rx_buf[buf_idx][i] + (1UL << 14)) >> 3);
  }

  if (sound_idle_count == 0U) {
    current_board->set_amp_enabled(true);
  }
  sound_idle_count = 2U;

  DMA1->LIFCR |= 0xF40;
  DMA1_Stream1->CR &= ~DMA_SxCR_EN;
  register_set(&DMA1_Stream1->M0AR, (uint32_t) tx_buf, 0xFFFFFFFFU);
  DMA1_Stream1->NDTR = SOUND_TX_BUF_SIZE;
  DMA1_Stream1->CR |= DMA_SxCR_EN;
}

void sound_tick(void) {
  if (sound_idle_count > 0U) {
    sound_idle_count--;
    if (sound_idle_count == 0U) {
      current_board->set_amp_enabled(false);
    }
  }
}

void sound_init(void) {
  REGISTER_INTERRUPT(BDMA_Channel0_IRQn, BDMA_Channel0_IRQ_Handler, 64U, FAULT_INTERRUPT_RATE_SOUND_DMA)

  // Init DAC
  register_set(&DAC1->MCR, 0U, 0xFFFFFFFFU);
  register_set(&DAC1->CR, DAC_CR_TEN1 | (6U << DAC_CR_TSEL1_Pos) | DAC_CR_DMAEN1, 0xFFFFFFFFU);
  register_set_bits(&DAC1->CR, DAC_CR_EN1);

  // Setup DMAMUX (DAC_CH1_DMA as input)
  register_set(&DMAMUX1_Channel1->CCR, 67U, DMAMUX_CxCR_DMAREQ_ID_Msk);

  // Setup DMA
  register_set(&DMA1_Stream1->PAR, (uint32_t) &(DAC1->DHR12R1), 0xFFFFFFFFU);
  register_set(&DMA1_Stream1->FCR, 0U, 0x00000083U);
  DMA1_Stream1->CR = (0b11UL << DMA_SxCR_PL_Pos) | (0b01UL << DMA_SxCR_MSIZE_Pos) | (0b01UL << DMA_SxCR_PSIZE_Pos) | DMA_SxCR_MINC | (1U << DMA_SxCR_DIR_Pos);

  // Init trigger timer (48kHz)
  register_set(&TIM7->PSC, 0U, 0xFFFFU);
  register_set(&TIM7->ARR, 2494U, 0xFFFFU);
  register_set(&TIM7->CR2, (0b10U << TIM_CR2_MMS_Pos), TIM_CR2_MMS_Msk);
  register_set(&TIM7->CR1, TIM_CR1_ARPE | TIM_CR1_URS, 0x088EU);
  TIM7->SR = 0U;
  TIM7->CR1 |= TIM_CR1_CEN;

  // sync both SAIs
  register_set(&SAI4->GCR, (0b10 << SAI_GCR_SYNCOUT_Pos), SAI_GCR_SYNCIN_Msk | SAI_GCR_SYNCOUT_Msk);
  register_set(&SAI1->GCR, (3U << SAI_GCR_SYNCIN_Pos), SAI_GCR_SYNCIN_Msk | SAI_GCR_SYNCOUT_Msk);

  // stereo audio in
  register_set(&SAI4_Block_B->CR1, SAI_xCR1_DMAEN | (0b00UL << SAI_xCR1_SYNCEN_Pos) | (0b100U << SAI_xCR1_DS_Pos) | (0b11U << SAI_xCR1_MODE_Pos), 0x0FFB3FEFU);
  register_set(&SAI4_Block_B->CR2, (0b001U << SAI_xCR2_FTH_Pos), 0xFFFBU);
  register_set(&SAI4_Block_B->FRCR, (31U << SAI_xFRCR_FRL_Pos), 0x7FFFFU);
  register_set(&SAI4_Block_B->SLOTR, (0b11UL << SAI_xSLOTR_SLOTEN_Pos) | (1UL << SAI_xSLOTR_NBSLOT_Pos) | (0b01UL << SAI_xSLOTR_SLOTSZ_Pos), 0xFFFF0FDFU); // NBSLOT definition is vague

  // init sound DMA (SAI4_B -> memory, double buffers)
  register_set(&BDMA_Channel0->CPAR, (uint32_t) &(SAI4_Block_B->DR), 0xFFFFFFFFU);
  register_set(&BDMA_Channel0->CM0AR, (uint32_t) sound_rx_buf[0], 0xFFFFFFFFU);
  register_set(&BDMA_Channel0->CM1AR, (uint32_t) sound_rx_buf[1], 0xFFFFFFFFU);
  BDMA_Channel0->CNDTR = SOUND_RX_BUF_SIZE;
  register_set(&BDMA_Channel0->CCR, BDMA_CCR_DBM | (0b01UL << BDMA_CCR_MSIZE_Pos) | (0b01UL << BDMA_CCR_PSIZE_Pos) | BDMA_CCR_MINC | BDMA_CCR_CIRC | BDMA_CCR_TCIE, 0xFFFFU);
  register_set(&DMAMUX2_Channel0->CCR, 16U, DMAMUX_CxCR_DMAREQ_ID_Msk); // SAI4_B_DMA
  register_set_bits(&BDMA_Channel0->CCR, BDMA_CCR_EN);

  // mic output
  register_set(&SAI4_Block_A->CR1, SAI_xCR1_DMAEN | (0b01 << SAI_xCR1_SYNCEN_Pos) | (0b100 << SAI_xCR1_DS_Pos) | (0b10 << SAI_xCR1_MODE_Pos), 0x0FFB3FEFU);
  register_set(&SAI4_Block_A->CR2, 0U, 0xFFFBU);
  register_set(&SAI4_Block_A->FRCR, (31U << SAI_xFRCR_FRL_Pos), 0x7FFFFU);
  register_set(&SAI4_Block_A->SLOTR, (0b11 << SAI_xSLOTR_SLOTEN_Pos) | (1U << SAI_xSLOTR_NBSLOT_Pos) | (0b01 << SAI_xSLOTR_SLOTSZ_Pos), 0xFFFF0FDFU); // NBSLOT definition is vague

  // init DFSDM for PDM mic
  DFSDM1_Channel0->CHCFGR1 = (6U << DFSDM_CHCFGR1_CKOUTDIV_Pos) | DFSDM_CHCFGR1_CHEN; // CH0 controls the clock
  DFSDM1_Channel0->CHCFGR1 |= DFSDM_CHCFGR1_DFSDMEN;
  DFSDM1_Channel3->CHCFGR1 |= (0b01 << DFSDM_CHCFGR1_SPICKSEL_Pos) | (0b00U << DFSDM_CHCFGR1_SITP_Pos) | DFSDM_CHCFGR1_CHEN; // SITP determines sample edge
  DFSDM1_Filter0->FLTFCR = (64U << DFSDM_FLTFCR_FOSR_Pos) | (4 << DFSDM_FLTFCR_FORD_Pos);
  DFSDM1_Filter0->FLTCR1 = (3U << DFSDM_FLTCR1_RCH_Pos) | DFSDM_FLTCR1_RCONT | DFSDM_FLTCR1_DFEN;
  DFSDM1_Filter0->FLTCR1 |= DFSDM_FLTCR1_RSWSTART;

  // init DMA for SAI output
  DMA1_Stream0->CR = DMA_SxCR_DBM | (8 << DMA_SxCR_CHSEL_Pos) | DMA_SxCR_PL_1 | DMA_SxCR_MSIZE_1 | (0b10UL << DMA_SxCR_PSIZE_Pos) | DMA_SxCR_CIRC;
  DMA1_Stream0->PAR = (uint32_t) &DFSDM1_Filter0->FLTRDATAR;
  DMA1_Stream0->M0AR = (uint32_t)&mic_rx_buf[0];
  DMA1_Stream0->M1AR = (uint32_t)&mic_rx_buf[1];
  DMA1_Stream0->CR |= DMA_SxCR_EN;
  DMAMUX1_Channel0->CC |= 101U;
  DFSDM1_Filter0->FLTCR2 |= DFSDM_FLTCR2_DMAEN;

  // enable all initted blocks
  register_set_bits(&SAI4_Block_B->CR1, SAI_xCR1_SAIEN);
  NVIC_EnableIRQ(BDMA_Channel0_IRQn);
}
