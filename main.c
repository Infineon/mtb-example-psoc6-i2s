/******************************************************************************
* File Name:   main.c
*
* Description: This is the source code for the I2S Example
*              for ModusToolbox.
*
* Related Document: See Readme.md
*
*
*******************************************************************************
* (c) (2019), Cypress Semiconductor Corporation. All rights reserved.
*******************************************************************************
* This software, including source code, documentation and related materials
* ("Software"), is owned by Cypress Semiconductor Corporation or one of its
* subsidiaries ("Cypress") and is protected by and subject to worldwide patent
* protection (United States and foreign), United States copyright laws and
* international treaty provisions. Therefore, you may use this Software only
* as provided in the license agreement accompanying the software package from
* which you obtained this Software ("EULA").
*
* If no EULA applies, Cypress hereby grants you a personal, non-exclusive,
* non-transferable license to copy, modify, and compile the Software source
* code solely for use in connection with Cypress's integrated circuit products.
* Any reproduction, modification, translation, compilation, or representation
* of this Software except as specified above is prohibited without the express
* written permission of Cypress.
*
* Disclaimer: THIS SOFTWARE IS PROVIDED AS-IS, WITH NO WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING, BUT NOT LIMITED TO, NONINFRINGEMENT, IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE. Cypress
* reserves the right to make changes to the Software without notice. Cypress
* does not assume any liability arising out of the application or use of the
* Software or any product or circuit described in the Software. Cypress does
* not authorize its products for use in any products where a malfunction or
* failure of the Cypress product may reasonably be expected to result in
* significant property damage, injury or death ("High Risk Product"). By
* including Cypress's product in a High Risk Product, the manufacturer of such
* system or application assumes all risk of such use and in doing so agrees to
* indemnify Cypress against all liability.
*******************************************************************************/

#include "cy_pdl.h"
#include "cyhal.h"
#include "cybsp.h"

#include "ak4954a.h"
#include "wave.h"

/*******************************************************************************
* Macros
********************************************************************************/
#define MI2C_TIMEOUT_MS     10u         /* in ms */
#define MCLK_FREQ_HZ        11250000u   /* in Hz */
#define MCLK_DUTY_CYCLE     50.0f       /* in %  */

/*******************************************************************************
* Function Prototypes
********************************************************************************/
cy_rslt_t mi2c_transmit(uint8_t reg_adrr, uint8_t data);
void i2s_isr_handler(void);

/*******************************************************************************
* Global Variables
********************************************************************************/
/* Master I2C variables */
cyhal_i2c_t mi2c;

const cyhal_i2c_cfg_t mi2c_cfg = {
    .is_slave        = false,
    .address         = 0,
    .frequencyhal_hz = 400000
};

const cy_stc_sysint_t i2s_isr_cfg = {
#if CY_IP_MXAUDIOSS_INSTANCES == 1
    .intrSrc = (IRQn_Type) audioss_interrupt_i2s_IRQn,
#else
    .intrSrc = (IRQn_Type) audioss_0_interrupt_i2s_IRQn,
#endif
    .intrPriority = CYHAL_ISR_PRIORITY_DEFAULT
};

uint32_t i2s_count = 0;

/* Master Clock PWM */
cyhal_pwm_t mclk_pwm;

/*******************************************************************************
* Function Name: main
********************************************************************************
* Summary:
*  The main function for the Cortex-M4 CPU does the following:
*   Initialization:
*   - Initializes all the hardware blocks
*   Do forever loop:
*   - Enters Sleep Mode.
*   - Check if the User Button was pressed. If yes, plays the audio track.
*
* Parameters:
*  void
*
* Return:
*  int
*
*******************************************************************************/
int main(void)
{
    cy_rslt_t result;

    /* Initialize the device and board peripherals */
    result = cybsp_init() ;
    if (result != CY_RSLT_SUCCESS)
    {
        CY_ASSERT(0);
    }

    /* Enable global interrupts */
    __enable_irq();

    /* Initialize the User LED */
    cyhal_gpio_init((cyhal_gpio_t) CYBSP_USER_LED, CYHAL_GPIO_DIR_OUTPUT, CYHAL_GPIO_DRIVE_STRONG, CYBSP_LED_STATE_ON);

    /* Initialize the User Button */
    cyhal_gpio_init((cyhal_gpio_t) CYBSP_USER_BTN, CYHAL_GPIO_DIR_INPUT, CYHAL_GPIO_DRIVE_PULLUP, CYBSP_BTN_OFF);
    cyhal_gpio_enable_event((cyhal_gpio_t) CYBSP_USER_BTN, CYHAL_GPIO_IRQ_FALL, CYHAL_ISR_PRIORITY_DEFAULT, true);

    /* Initialize the Master Clock with a PWM */
    cyhal_pwm_init(&mclk_pwm, (cyhal_gpio_t) P5_0, NULL);
    cyhal_pwm_set_duty_cycle(&mclk_pwm, MCLK_DUTY_CYCLE, MCLK_FREQ_HZ);
    cyhal_pwm_start(&mclk_pwm);

    /* Wait for the MCLK to clock the audio codec */
    Cy_SysLib_Delay(1);

    /* Initialize the I2C Master */
    cyhal_i2c_init(&mi2c, CYBSP_I2C_SDA, CYBSP_I2C_SCL, NULL);
    cyhal_i2c_configure(&mi2c, &mi2c_cfg);

    /* Initialize the I2S interrupt */
    Cy_SysInt_Init(&i2s_isr_cfg, i2s_isr_handler);
    NVIC_EnableIRQ(i2s_isr_cfg.intrSrc);   

    /* Initialize the I2S */
    Cy_I2S_Init(CYBSP_I2S_HW, &CYBSP_I2S_config);
    Cy_I2S_ClearTxFifo(CYBSP_I2S_HW);
    /* Put at least one frame into the Tx FIFO */
    Cy_I2S_WriteTxData(CYBSP_I2S_HW, 0UL);
    Cy_I2S_WriteTxData(CYBSP_I2S_HW, 0UL);
    /* Enable the I2S interface */
    Cy_I2S_EnableTx(CYBSP_I2S_HW);
    /* Clear possible pending interrupts */
    Cy_I2S_ClearInterrupt(CYBSP_I2S_HW, CY_I2S_INTR_TX_TRIGGER);
    /* Enable I2S interrupts */
    Cy_I2S_SetInterruptMask(CYBSP_I2S_HW, CY_I2S_INTR_TX_TRIGGER);

    /* Configure the AK494A codec and enable it */
    ak4954a_init(mi2c_transmit);
    ak4954a_activate();

    for(;;)
    {
        cyhal_system_sleep();       

        /* Check if the button was pressed */
        if (cyhal_gpio_read(CYBSP_USER_BTN) == CYBSP_BTN_PRESSED)
        {
            /* Invert the USER LED state */
            cyhal_gpio_toggle((cyhal_gpio_t) CYBSP_USER_LED);

            Cy_SysLib_Delay(500);

            /* Check if the I2S ISR is disabled */
            if (!NVIC_GetEnableIRQ(i2s_isr_cfg.intrSrc))
            {
                /* Turn ON the USER LED */
                cyhal_gpio_write((cyhal_gpio_t) CYBSP_USER_LED, CYBSP_LED_STATE_ON);

                /* Restart the audio track counter */
                i2s_count = 0;

                /* Re-enable the I2S ISR */
                NVIC_EnableIRQ(i2s_isr_cfg.intrSrc);
            }
        }
    }
}

/*******************************************************************************
* Function Name: mi2c_transmit
********************************************************************************
* Summary:
*  I2C Master function to transmit data to the given address.
*
* Parameters:
*  reg_addr: address to be updated
*  data: 8-bit data to be written in the register
*
* Return:
*  cy_rslt_t - I2C master transaction error status. 
*              Returns CY_RSLT_SUCCESS if succeeded.
*
*******************************************************************************/
cy_rslt_t mi2c_transmit(uint8_t reg_addr, uint8_t data)
{
    cy_rslt_t result;
    uint8_t buffer[AK4954A_PACKET_SIZE];
    
    buffer[0] = reg_addr;
    buffer[1] = data;

    /* Send the data over the I2C */
    result = cyhal_i2c_master_write(&mi2c, AK4954A_I2C_ADDR, buffer, 2, MI2C_TIMEOUT_MS, true);

    return result;
}

/*******************************************************************************
* Function Name: i2s_isr_handler
****************************************************************************//**
*
* I2S Interrupt Handler Implementation. Feed the I2S internal FIFO with audio
* data.
*  
*******************************************************************************/
void i2s_isr_handler(void)
{   
    /* Write data for the left side */
    Cy_I2S_WriteTxData(CYBSP_I2S_HW, (uint16) wave_data[i2s_count]);
    
    /* Write data for the right side */
    Cy_I2S_WriteTxData(CYBSP_I2S_HW, (uint16) wave_data[i2s_count]);
            
    /* If the end of the wave data is reached, stop the ISR, otherwise, increment counter */
    if (i2s_count < WAVE_SIZE)
    {
       i2s_count++; 
    }   
    else
    {
        /* Disable the ISR */
        NVIC_DisableIRQ(i2s_isr_cfg.intrSrc);      

        /* Turn OFF the USER LED */
        cyhal_gpio_write((cyhal_gpio_t) CYBSP_USER_LED, CYBSP_LED_STATE_OFF);
    }

    /* Clear I2S Interrupt */
    Cy_I2S_ClearInterrupt(CYBSP_I2S_HW, CY_I2S_INTR_TX_TRIGGER);
}

/* [] END OF FILE */
