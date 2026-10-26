#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef struct dma2d_descriptor { unsigned vb_size,hb_length,owner,suc_eof;void *buffer;struct dma2d_descriptor *next; } dma2d_descriptor_t;
typedef struct {struct {struct {uint32_t val;} in_int_raw;} in_channel[1];bool clock;uintptr_t tx_head,rx_head;} dma2d_dev_t;
extern dma2d_dev_t mock_hw;
void mock_dma_reset(void);
void mock_dma_start(void);
#define DMA2D_LL_GET_HW(n) (&mock_hw)
#define __DECLARE_RCC_ATOMIC_ENV __attribute__((unused)) mock_atomic_env
#define DMA2D_LL_EVENT_RX_DESC_ERROR 1u
#define DMA2D_LL_EVENT_RX_ERR_EOF 2u
#define DMA2D_LL_EVENT_RX_SUC_EOF 4u
#define DMA2D_TRIG_PERIPH_M2M 0
#define DMA2D_DATA_BURST_LENGTH_128 0
#define DMA2D_MACRO_BLOCK_SIZE_NONE 0
#define DMA2D_CSC_TX_NONE 0
#define DMA2D_CSC_RX_NONE 0
#define dma2d_ll_is_bus_clock_enabled(n) (mock_hw.clock)
#define dma2d_ll_enable_bus_clock(n,on) (mock_hw.clock=(on))
#define dma2d_ll_reset_register(n) mock_dma_reset()
#define dma2d_ll_tx_reset_channel(h,n) mock_dma_reset()
#define dma2d_ll_rx_reset_channel(h,n) mock_dma_reset()
#define dma2d_ll_tx_set_desc_addr(h,n,p) ((h)->tx_head=(p))
#define dma2d_ll_rx_set_desc_addr(h,n,p) ((h)->rx_head=(p))
#define dma2d_ll_rx_clear_interrupt_status(h,n,v) ((h)->in_channel[0].in_int_raw.val=0)
#define dma2d_ll_tx_start(h,n) mock_dma_start()
#define dma2d_ll_hw_enable(...) ((void)0)
#define dma2d_ll_tx_enable_interrupt(...) ((void)0)
#define dma2d_ll_rx_enable_interrupt(...) ((void)0)
#define dma2d_ll_tx_clear_interrupt_status(...) ((void)0)
#define dma2d_ll_tx_connect_to_periph(...) ((void)0)
#define dma2d_ll_rx_connect_to_periph(...) ((void)0)
#define dma2d_ll_tx_enable_dscr_port(...) ((void)0)
#define dma2d_ll_rx_enable_dscr_port(...) ((void)0)
#define dma2d_ll_tx_enable_owner_check(...) ((void)0)
#define dma2d_ll_rx_enable_owner_check(...) ((void)0)
#define dma2d_ll_tx_enable_eof_mode(...) ((void)0)
#define dma2d_ll_tx_enable_auto_write_back(...) ((void)0)
#define dma2d_ll_rx_set_auto_return_owner(...) ((void)0)
#define dma2d_ll_tx_enable_reorder(...) ((void)0)
#define dma2d_ll_rx_enable_reorder(...) ((void)0)
#define dma2d_ll_tx_enable_page_bound_wrap(...) ((void)0)
#define dma2d_ll_rx_enable_page_bound_wrap(...) ((void)0)
#define dma2d_ll_tx_enable_descriptor_burst(...) ((void)0)
#define dma2d_ll_rx_enable_descriptor_burst(...) ((void)0)
#define dma2d_ll_tx_set_data_burst_length(...) ((void)0)
#define dma2d_ll_rx_set_data_burst_length(...) ((void)0)
#define dma2d_ll_tx_set_macro_block_size(...) ((void)0)
#define dma2d_ll_rx_set_macro_block_size(...) ((void)0)
#define dma2d_ll_tx_configure_color_space_conv(...) ((void)0)
#define dma2d_ll_rx_configure_color_space_conv(...) ((void)0)
#define dma2d_ll_rx_start(...) ((void)0)
