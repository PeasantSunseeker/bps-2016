/*
 * Battery Protection Software for BPS2015 PCB Development
 * Based on previous Sunseeker code.
 * Some functions are based on code originally written by
 * Tritium Pty Ltd. See functional modules for Tritium code.
 *
 * Western Michigan University
 * Sunseeker BPS 2015 Version 1
 * Created by Scott Haver November 2015
 *
 * Modified DEc 2016 - BJB
 * Add debug check allowances
 * Add CAN Bus operational check in Normal Mode
 * Modify Timing Daley to start up
 *
 */


/*
 * RS232 COMMANDS:
 *
 * 1) battery temps
 * 2) battery volts
 * 3) battery current
 * 4) battery state
 */

/*
 * batt_Kill errors
 * LTC Status Error 0x11, 0x12, 0x13
 * LTC COMM Error 0x20
 * Max I Discharge 0x30
 * Max I Discharge Temp 0x50
 * Max I Charge 0X40
 * Max I Charge Temp 0x60
 * Temp Sensor Disconnect 0x70
 * DC CAN Mseeage Failure 0x80
 */

#include <stdio.h>
#include <stdlib.h>
#include <msp430x54xa.h>
#include <ctype.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include "BPSmain.h"
#include "RS232.h"
#include "ad7739_func.h"
#include "LTC6803.h"
#include "can.h"

#define MAX_TEMP_DISCHARGE 		0x002da2b3 		//59 Degree C
#define MAX_TEMP_CHARGE   		0x004582EA		//45 Degree C
#define MIN_TEMP_NOSENSOR  		0x00B00000		//Therm not connected
//0x2DA2B3 is 60 deg (140 F) !00k and 330k resistors, 100k NTC device
//0x34BCFA is 55 deg (131 F)
//0x4582EA is 45 deg (113 F)
//0x6F286B is 25 deg ( 77 F)
//0xA05D1A is  0 deg ( 32 F)
//0xC47711 is Therm not connected

#define MAX_CURRENT_DISCHARGE	+80200.0		//80.0A    @ 2.45 volts across batt shunt
#define MAX_CURRENT_CHARGE		-19500.0		//-19.5A @ .62 volts across batt shunt

//LTC Variables
volatile unsigned char ltc1_FLGR[3], ltc1_errflag = 0x00;	//set if LTC1 error
volatile unsigned char ltc2_FLGR[3], ltc2_errflag = 0x00; 	//set if LTC2 error
volatile unsigned char ltc3_FLGR[3], ltc3_errflag = 0x00; 	//set if LTC3 error
unsigned char ltc_error = 0;					//combined error calc
volatile unsigned char batt1,batt2,batt3;		//stores return value of flag read
volatile unsigned char sc_batt_error = 0x00;	// self check status error

volatile unsigned char cm_flag = FALSE;
volatile unsigned char status_flag = FALSE;		//status flag set on timer B
volatile unsigned char batt_KILL = FALSE;		//set to isolate batt
volatile unsigned char batt_ERR = 0x00 ;		//Cause of batt_KILL
volatile unsigned char capture_err = 0x00 ;		//Cause of batt_KILL
volatile unsigned char int_op1_flag = 0x00;		//interrupt operational flags
volatile unsigned char int_op2_flag = 0x00;		//interrupt operational flags
volatile unsigned char adc_rdy_flag = 0x00;		//interrupt operational flags

int mode_count = 0;								//used for sequencing
int mode_dwell_count = 0;						//used for sequencing
unsigned int ltc1_cv[12];						//holds ltc1 cell volts
unsigned int ltc2_cv[12];						//holds ltc2 cell volts
unsigned int ltc3_cv[12];						//holds ltc3 cell volts

//ADC Temperature Variables
volatile signed long adc_voltage[8*7];		//stores adc values
volatile signed long max_temp_voltage;

volatile unsigned char adc_stat[8*7];		//stores adc status
volatile unsigned char max_temp_index;

volatile unsigned char ch;						//used for ch switching of adc
volatile unsigned char dev;						//used for dev switching of
volatile unsigned char i;						//used for counting
volatile unsigned char temp_flag_bus1 = FALSE;		//used for temp measurement timing
volatile unsigned char temp_flag_bus2 = FALSE;		//used for temp measurement timing
volatile unsigned char temp_status = 0xFF; 	// 0 is OK, 1 is >45C, 2 is >60C, 3 is no them, 4 is ref err
volatile unsigned char selfcheck_temp_error = TRUE;

//ADC MISC Current Variables
volatile long diff_ref, diff_shunt, current_dvolt;
volatile float current;					//keeps track of current direction across batt shunt

// CAN Communication Variables
volatile unsigned char send_can = FALSE;	//used for CAN transmission timing
volatile unsigned char can_full = FALSE;	//used for CAN transmission status
volatile unsigned char cancomm_flag = FALSE;	//used for CAN transmission timing
volatile unsigned char ac_charge_mode = FALSE;	//used for CAN transmission
volatile unsigned char dc_charge_mode = FALSE;	//used for CAN transmission
volatile unsigned char charge_mode = 0x00;	//used for CAN transmission

volatile int 	max_v_cell, ltc_max;
volatile long	bat_voltage;
volatile float 	max_voltage;
volatile int 	min_v_cell, ltc_min;
volatile float 	min_voltage;
volatile long	max_temp_val;
volatile int	max_temp_idx;
volatile unsigned char comms_event_count = 0;
volatile unsigned int can_err_cnt = 0x0000;
volatile int cancheck_flag;
volatile unsigned char 	can_CANINTF, can_FLAGS[3];

volatile int switches_out_dif, switches_dif_save;
volatile int switches_out_new, switches_new;
volatile int can_DC_rcv_count, old_DC_rcv_count;
volatile char can_car_enable = FALSE;
volatile char can_start_precharge = FALSE;
volatile char precharge_complete = FALSE;

volatile char AC_char3, AC_char2, AC_char1, AC_char0;
volatile long AC_Serial_No;

//PreCharge Variables
unsigned long SIG1;								//measures BATT voltage
unsigned long SIG2;								//measures precharge shunt resistor voltage
unsigned long SIG3;								//measures voltage after MC contactor
unsigned long compare_sig1;						//stores signal comparison value
unsigned long compare_sig2;						//stores signal comparison value
volatile char PC_Complete = FALSE;				//TRUE when caps are charged
volatile char MC_Complete = FALSE;				//TRUE when MC contactor is closed

//RS232 Variables
char command[32];								//stores rs232 commands
char buff[32];									//buff array to hold sprintf string
unsigned char rs232_count = 0;					//counts characters received
char batt_temp[32] = "battery temps\r";			//command to read batt temperatures
char batt_temp_status = 0;						//TRUE if battery temps is sent
char batt_volt[32] = "battery volts\r";			//command to read cell volts
char batt_volt_status = 0;						//TRUE if battery volts is sent
char batt_current[32] ="battery current\r";		//command to read battery current
char batt_current_status = 0;					//TRUE if battery current is sent
char batt_state[32] ="battery state\r";		//command to read battery current
char batt_state_status = 0;					//TRUE if battery current is sent
float volt;										//variable to store adc volt conv
float temp;										//temperature calculation
float max_temp;
int temp1;										//integer value
int temp2;										//tenths place
int temp3;										//hundredths place

unsigned int err_mode_cnt = 7*4;

// BPS Debugging
volatile char check_TEMP=TRUE;
volatile char check_LTC1=TRUE;
volatile char check_LTC2=TRUE;
volatile char check_LTC3=TRUE;
volatile char check_VOLTAGE1=TRUE;
volatile char check_VOLTAGE2=TRUE;
volatile char check_VOLTAGE3=TRUE;
volatile char check_CURRENT=TRUE;
volatile char check_PRECHARGE=TRUE;
volatile char check_CAN=FALSE;
volatile char check_SC=TRUE;

// ADC Indexes
// Temp Locations: 1,2,3,4,5,6,7,  9,10,11,12,13,14,15,  17,18,19,20,21,22,23,
// Temp Locations: 25,26,27,28,29,30,31  33,34,35,36,37,38,39, 46,47
// Temp Locations: 50, 51

// 2.5V Locations: 0,8,16,24,32,40,48
// MC PC Locations: 0,8,16,24,32,40,48
// SCAP Ref & Shunt Location: 45 & 41
// SCAP PC Locations: 42, 43, 44
// Bat Ref & Shunt Location: 49 & 52
// MC PC Locations: 53, 54, 55

/*=================================== **MAIN** =========================================*/
//////////////////////////////////////////////////////////////////////////////////////////

int main(void)
{
	unsigned int i;

	enum MODE
	{
		INITIALIZE,
		SELFCHECK,
		BPSREADY,
		ARRAYREADY,
		CANCHECK,
		PRECHARGE,
		NORMALOP,
		CHARGE,
		ERRORMODE
	}bpsMODE;

	WDTCTL = WDTPW | WDTHOLD | WDTSSEL__ACLK; 	// Stop watchdog timer to prevent time out reset
	_DINT();     		    					//disables interrupts

	//open all relays
	relay_mcpc_open;
	ext_relay_mcpc_open;
	relay_mc_open;
	delay();
	relay_batt_open;
	delay();
	relay_array_open;

	bps_strobe_off;

	bpsMODE = INITIALIZE;
	DR_LED0_ON;								//INIT LED ON

	io_init();									//enable IO pins
	clock_init();								//Configure HF and LF clocks
	timerB_init();								//init timer B
	
	BPS2PC_init();								//init RS232
	canspi_init();
	can_init();
	can_DC_rcv_count = 0;
	old_DC_rcv_count = 0;

	status_flag = TRUE;

	while(TRUE)								//loop forever
	{
		if(status_flag)
		{
			status_flag = FALSE;
			mode_count++;
			mode_dwell_count++;
			batt_KILL = FALSE;
			batt_ERR = 0x00;

			if(bpsMODE == INITIALIZE)
			{
				WDTCTL = WDTPW | WDTHOLD | WDTSSEL__ACLK; 	// Stop watchdog timer to prevent time out reset
				_DINT();     		    					//disables interrupts

				//open all relays
				relay_mcpc_open;
				ext_relay_mcpc_open;
				relay_mc_open;
				delay();
				relay_batt_open;
				delay();
				relay_array_open;

				bps_strobe_off;

				send_can = FALSE;

				DR_LED0_ON;								//INIT LED ON

				BPS2PC_init();								//init RS232
				canspi_init();
				can_init();

				//adc bus1 initializations
				adc_bus1_spi_init();
				adc_bus1_init();
				adc_bus1_selfcal(1);
				adc_bus1_read_convert(0,1);
				adc_bus1_selfcal(2);
				adc_bus1_read_convert(0,2);
				adc_bus1_selfcal(3);
				adc_bus1_read_convert(0,3);
				//adc bus2 initializations
				adc_bus2_spi_init();
				adc_bus2_init();
				adc_bus2_selfcal(1);
				adc_bus2_read_convert(0,1);
				adc_bus2_selfcal(2);
				adc_bus2_read_convert(0,2);
				adc_bus2_selfcal(3);
				adc_bus2_read_convert(0,3);
				//adc misc initializations
				adc_misc_spi_init();
				adc_misc_init();
				adc_misc_selfcal();
				adc_misc_read_convert(0);

				i = 50000;									// SW Delay
				do i--;
				while (i != 0);
				// Uncertain why these are each done twice ... bjb
				//LTC1 Configure
				LTC1_init();
				LTC1_init();
				ltc1_errflag = 0x00;
				//LTC2 Configure
				LTC2_init();
				LTC2_init();
				ltc2_errflag = 0x00;
				//LTC3 Configure
				LTC3_init();
				LTC3_init();
				ltc3_errflag = 0x00;
				
				bpsMODE = SELFCHECK;
				mode_dwell_count = 0;
				DR_LED0_OFF;
				DR_LED1_ON;

				mode_count = 0;
				mode_dwell_count = 0;

				// Parallel Pin Interrupts
				//    P1IE  = BUTTON1 | BUTTON2; // Enable Interrupts
				//    P1IE  |= CAN_RX0n | CAN_RX1n;
				//    P1IFG &= ~(BUTTON1 | BUTTON2 | CAN_RX0n | CAN_RX1n);

				P2IFG &= ~(CAN_INTn|ADC1_RDYn|ADC2_RDYn|ADC3_RDYn|ADC4_RDYn|ADC5_RDYn|ADC6_RDYn|ADC7_RDYn);
				P2IE |= ADC1_RDYn |ADC2_RDYn |ADC3_RDYn |ADC4_RDYn | ADC5_RDYn | ADC6_RDYn | ADC7_RDYn;

			    /*Enable Interrupts*/
				UCA3IE |= UCRXIE;			//RS232 receive interrupt
				WDTCTL = WDT_ARST_1000; 	// Stop watchdog timer to prevent time out reset
				__bis_SR_register(GIE); 	//enable global interrupts
				__no_operation();			//for compiler

			}
			else if(bpsMODE == ERRORMODE)
			{
				//look for CAN message to end errormode
				err_mode_cnt--;
				if(err_mode_cnt ==0x00)
				{
					bps_strobe_tog;
					err_mode_cnt = 7*4;
				}

			}
			else	//normal periodic sequence based on mode_count
			{
				//batt1 status - O/U voltage
				batt1 = LTC1_Read_Config();
				batt1 |= LTC1_Read_Flags();
				if((batt1 != 0x00) && (bpsMODE != SELFCHECK) && (check_LTC1 ))
				{
					batt_KILL = TRUE;
					batt_ERR = 0x11;
					batt1=0x00;
				}
				//batt2 status - O/U voltage
				batt2 = LTC2_Read_Config();
				batt2 |= LTC2_Read_Flags();
				if((batt2 != 0x00) && (bpsMODE != SELFCHECK) && (check_LTC2))
				{
					batt_KILL = TRUE;
					batt_ERR = 0x12;
					batt2=0x00;
				}
				//batt3 status - O/U voltage
				batt3 = LTC3_Read_Config();
				batt3 |= LTC3_Read_Flags();
				if((batt3 != 0x00) && (bpsMODE != SELFCHECK) && (check_LTC3))
				{
					batt_KILL = TRUE;
					batt_ERR = 0x13;
					batt3=0x00;
				}
				if (check_SC) {
					sc_batt_error = batt1 | batt2 |batt3;
				}
				else
				{
					sc_batt_error = 0x00;
				}


				// Periodic Measurements
				adc_misc_contconv_start(0x00);	//Shunt and other values

				switch(mode_count)
				{
					case 1:
					  ltc1_errflag = 0x00;
					  ltc2_errflag = 0x00;
					  ltc3_errflag = 0x00;
					  adc_bus1_contconv_start(0x00,1);
					  adc_bus2_contconv_start(0x00,1);

					break;
					case 2:
					  LTC1_Start_ADCCV();
					break;
					case 3:
					  batt1 = LTC1_Read_Voltages();
					  if (batt1 != 0x00 && check_VOLTAGE1)
					  {
						ltc1_errflag = TRUE;
					  }
					break;
					case 4:
					  LTC2_Start_ADCCV();
					break;
					case 5:
					  batt2 = LTC2_Read_Voltages();
					  if (batt2 != 0x00 && check_VOLTAGE2)
					  {
						 ltc2_errflag = TRUE;
					  }
					break;
					case 6:
					 LTC3_Start_ADCCV();
					break;
					case 7:
					  batt3 = LTC3_Read_Voltages();
					  if (batt3 != 0x00 && check_VOLTAGE3)
					  {
						  ltc3_errflag = TRUE;
					  }
					break;
					case 8:
						asm(" nop");
					break;
					default:
					  mode_count = 0;
					break;
				}	// END Switch mode_count

				ltc_error = ltc1_errflag | ltc2_errflag | ltc3_errflag;

				if (ltc_error != 0x00 && (bpsMODE != SELFCHECK))
				{
					batt_KILL = TRUE; // LTC Error
					batt_ERR = 0x20;
					ltc_error = FALSE;
				}
				if (mode_count == 0x08)
				{
					mode_count = 0;

					switch(bpsMODE)
					{
					case SELFCHECK:
						if(sc_batt_error || ltc_error)
						{
							batt_KILL = FALSE;
							batt_ERR = 0x00;
							//reset  batt_KILL
							LED_NORMALOP_OFF;									//LED NORMALOP OFF
							// why multiple times? - bjb
							//re-init ltcs reset flags
							LTC1_init();
							LTC2_init();
							LTC3_init();
							ltc1_errflag = 0x00;
							ltc2_errflag = 0x00;
							ltc3_errflag = 0x00;
							//adc bus1 initializations							//re-init adcs and calibrate
/*							adc_bus1_spi_init();
							adc_bus1_init();
							adc_bus1_selfcal(1);
							adc_bus1_read_convert(0,1);
							adc_bus1_selfcal(2);
							adc_bus1_read_convert(0,2);
							adc_bus1_selfcal(3);
							adc_bus1_read_convert(0,3);
							//adc bus2 initializations
							adc_bus2_spi_init();
							adc_bus2_init();
							adc_bus2_selfcal(1);
							adc_bus2_read_convert(0,1);
							adc_bus2_selfcal(2);
							adc_bus2_read_convert(0,2);
							adc_bus2_selfcal(3);
							adc_bus2_read_convert(0,3);
							//adc misc initializations
							adc_misc_spi_init();
							adc_misc_init();
							adc_misc_selfcal();
							adc_misc_read_convert(0);
*/
						}
						else if (selfcheck_temp_error == FALSE)
						{
							bpsMODE = BPSREADY;
							mode_dwell_count = 0;
							P6OUT &= ~(LED3|LED2);		//DR LED 0x3
							P6OUT |=  (LED5|LED4);		//
							LED_ERROR_OFF;
							// canspi_init();
							// can_init();
						}
//						else selfcheck_temp_error = FALSE;
					break;

					case BPSREADY:
						relay_batt_close;		//BPS Power Enabled
						if(mode_dwell_count>=8)
						{
							bpsMODE = ARRAYREADY;
							mode_dwell_count = 0;
							P6OUT &= ~(LED4);				//DR LED 0x4
							P6OUT |=  (LED5|LED3|LED2);		//
						}

					break;

					case ARRAYREADY:
						relay_array_close;		//Array Connection Enabled
						send_can = TRUE;		//Start CAN transmissions
						can_DC_rcv_count = 0;
						old_DC_rcv_count = 0;

						if(mode_dwell_count>=8)
						{
							bpsMODE = CANCHECK;
							mode_dwell_count = 0;
							P6OUT &= ~(LED4|LED2);		//DR LED 0x5
							P6OUT |=  (LED5|LED3);		//

						}

					break;

					case CANCHECK:
						//check for CAN message start PRECHARGE
						// If array controller in charge mode
						// If driver controller in charge mode
						// enter the CHARGE operational mode

						if(mode_dwell_count>=16)
						{
							if(dc_charge_mode)
							{
								relay_mcpc_open;
								relay_mc_open;
								ext_relay_mcpc_open;
								charge_mode |= 0x01;
								bpsMODE = CHARGE;
								mode_dwell_count = 0;
								P6OUT &= ~(LED5);		//DR LED 0x8
								P6OUT |=  (LED4|LED3|LED2);		//

							}
							else if(ac_charge_mode)
							{
								relay_mcpc_open;
								relay_mc_open;
								ext_relay_mcpc_open;
								charge_mode |= 0x02;
								bpsMODE = CHARGE;
								mode_dwell_count = 0;
								P6OUT &= ~(LED5);		//DR LED 0x8
								P6OUT |=  (LED4|LED3|LED2);		//

							}
							else if ((can_start_precharge || can_car_enable) || (check_CAN == 0))
							{
								can_start_precharge = FALSE;
								relay_mcpc_close;										//close MCPC contactor
								ext_relay_mcpc_close;									//close external pc relay to read signals
								bpsMODE = PRECHARGE;
								LED_ERROR_TOG;
								mode_dwell_count = 0;
								P6OUT &= ~(LED4|LED3);		//DR LED 0x6
								P6OUT |=  (LED5|LED2);		//
							}
							mode_dwell_count = 0;
						}
					break;

					case CHARGE:
						//check for CAN message start PRECHARGE
						// If array controller in charge more
						// If driver controller in charge mode
						// Insure precharge and motor controller

						switch(charge_mode)
						{
							case 0x10:
								LED_NORMALOP_TOG;
							break;
							case 0x01:
								if (dc_charge_mode)
								{
									LED_NORMALOP_TOG;
								}
								else
								{
									bpsMODE = CANCHECK;
									mode_dwell_count = 0;
									P6OUT &= ~(LED4|LED2);		//DR LED 0x5
									P6OUT |=  (LED5|LED3);		//
									PC_Complete = FALSE;
									MC_Complete = FALSE;
									charge_mode = 0x00;
								}
							break;
							case 0x02:
								if (ac_charge_mode)
								{
									LED_NORMALOP_TOG;
								}
								else
								{
									bpsMODE = CANCHECK;
									mode_dwell_count = 0;
									P6OUT &= ~(LED4|LED2);		//DR LED 0x5
									P6OUT |=  (LED5|LED3);		//
									PC_Complete = FALSE;
									MC_Complete = FALSE;
									charge_mode = 0x00;
								}
							break;

						}
					break;

					case PRECHARGE:

						// Conversion Continuously Running
						// Note that ADC misc channels 5, 6, and 7 could be turned off before after this state
						// 2.5V Locations: 48
						// Bat Ref & Shunt Location: 49 & 52
						// MC PC Locations: 53, 54, 55

//						adc_misc_contconv_start(5);						//battery signal 			(SIGNAL 1)
//						adc_misc_contconv_start(6);						//precharge resistor signal (SIGNAL 2)
//						adc_misc_contconv_start(7);						//motor contactor signal    (SIGNAL 3)

//						SIG1 = adc_misc_read_convert(5);
//						SIG2 = adc_misc_read_convert(6);
//						SIG3 = adc_misc_read_convert(7);

						SIG1 = adc_voltage[53];
						SIG2 = adc_voltage[54];
						SIG3 = adc_voltage[55];

						compare_sig1 = abs(SIG1 - SIG2);					//compare SIG1 & SIG2
						compare_sig2 = abs(SIG2 - SIG3);					//compare SIG2 & SIG3


						if(mode_dwell_count>=8)
						{
							if(check_PRECHARGE == 0)
							{
								PC_Complete = TRUE;
								MC_Complete = TRUE;
							}

						if(PC_Complete == FALSE)								//if caps are not charged
						{
							//wait for valid reading SIG1 92.4 to 147 V
							// R Divider ~0.01525	--> 0x00E5C1D5 to 0x00906B35
							//
							if (SIG1 > 0x00900000)
							{
//								compare_sig = abs(SIG1 - SIG2);					//compare SIG1 & SIG2

								if(compare_sig1 < 0x040000 && SIG2 > 0x008C0000)	//check comparison is small and SIG2 is not noise
								{
									PC_Complete = TRUE;
								}
								else
								{
									PC_Complete = FALSE;
								}
							}
						}
						else if(MC_Complete == FALSE && PC_Complete == TRUE)	//only run loop when PC_Complete = TRUE
						{
							if (SIG2 > 0x00900000)
							{	//wait for valid reading SIG1
//								compare_sig = abs(SIG2 - SIG3);					//compare SIG2 & SIG3

								if(compare_sig2 < 0x040000 && SIG3 > 0x008C0000)	//ensure motor contactor is closed
								{
									MC_Complete = TRUE;
								//PC is complete at this point
									relay_mc_close;				  				//close MC contactor
									ext_relay_mcpc_open;						//open external PC relay
									relay_mcpc_open;			  				//open MCPC contactor
									bpsMODE = NORMALOP;
									LED_ERROR_TOG;
									mode_dwell_count = 0;
									P6OUT &= ~(LED4|LED3|LED2);		//DR LED 0x7
									P6OUT |=  (LED5);		//

									// Transmit Precharge CAN Message
									can.address = BP_CAN_BASE + BP_PCDONE;
									can.data.data_u8[7] = 'B';
									can.data.data_u8[6] = 'P';
									can.data.data_u8[5] = 'v';
									can.data.data_u8[4] = '1';
									can.data.data_u32[0] = DEVICE_SERIAL;
									cancheck_flag=can_transmit();
									if(cancheck_flag == 1) can_init();
								}
								else
								{
									MC_Complete = FALSE;
								}
							}

						}
						else if(MC_Complete == TRUE && PC_Complete == TRUE)
						{
							//PC is complete at this point
								relay_mc_close;				  				//close MC contactor
								ext_relay_mcpc_open;						//open external PC relay
								relay_mcpc_open;			  				//open MCPC contactor
								bpsMODE = NORMALOP;
								LED_ERROR_TOG;
								mode_dwell_count = 0;
								P6OUT &= ~(LED4|LED3|LED2);		//DR LED 0x7
								P6OUT |=  (LED5);		//

								// Transmit Precharge CAN Message
								can.address = BP_CAN_BASE + BP_PCDONE;
								can.data.data_u8[7] = 'B';
								can.data.data_u8[6] = 'P';
								can.data.data_u8[5] = 'v';
								can.data.data_u8[4] = '1';
								can.data.data_u32[0] = DEVICE_SERIAL;
								cancheck_flag=can_transmit();
								if(cancheck_flag == 1) can_init();

						}
						}
					break;

					case NORMALOP:
						LED_NORMALOP_TOG;
						// Insure that DC CAN messages are continuously received
						// check every ~4 seconds when allowed (shutdown within 8)
						if( mode_dwell_count >= 32 )
						{
							mode_dwell_count = 0;
							if ((can_DC_rcv_count == old_DC_rcv_count) && (check_CAN))
								{
									batt_KILL = TRUE;
									batt_ERR = 0x80;
								}
							old_DC_rcv_count = can_DC_rcv_count;
						}
					break;

					}//end switch(bpsMODE)

				}//end if(mode_count == 0x08)

			}//end else

		}//end if(status_flag)


		//////////////////////////CHECK ADC TEMP LIMITS//////////////////////////////////////////
		// Read ADC values when ready
	    if(adc_rdy_flag & ADC1_RDYn)
	    {
	    	adc_bus1_idle(0,1);
	      	adc_rdy_flag &= ~ADC1_RDYn;
	      	adc_bus1_read_convert(0,1);

	      	adc_bus1_contconv_start(0x00,2);
	    }

	    if(adc_rdy_flag & ADC2_RDYn)
	    {
	    	adc_bus1_idle(0,2);
	      	adc_rdy_flag &= ~ADC2_RDYn;
	      	adc_bus1_read_convert(0,2);

	      	adc_bus1_contconv_start(0x00,3);
	    }

	    if(adc_rdy_flag & ADC3_RDYn)
	    {
	    	adc_bus1_idle(0,3);
	      	adc_rdy_flag &= ~ADC3_RDYn;
	      	adc_bus1_read_convert(0,3);

	      	temp_flag_bus1=TRUE;
	    }

	    if(adc_rdy_flag & ADC4_RDYn)
	    {
	    	adc_bus2_idle(0,1);
	      	adc_rdy_flag &= ~ADC4_RDYn;
	      	adc_bus2_read_convert(0,1);

	      	adc_bus2_contconv_start(0x00,2);
	    }

	    if(adc_rdy_flag & ADC5_RDYn)
	    {
	    	adc_bus2_idle(0,2);
	      	adc_rdy_flag &= ~ADC5_RDYn;
	      	adc_bus2_read_convert(0,2);

	      	adc_bus2_contconv_start(0x00,3);
	    }

	    if(adc_rdy_flag & ADC6_RDYn)
	    {
	    	adc_bus2_idle(0,3);
	      	adc_rdy_flag &= ~ADC6_RDYn;
	      	adc_bus2_read_convert(0,3);

	      	temp_flag_bus2=TRUE;

	    }

		if(adc_rdy_flag & ADC7_RDYn)
		{
		   	adc_misc_idle(0);
		   	adc_rdy_flag &= ~ADC7_RDYn;
		   	adc_misc_read_convert(0);

		   	cm_flag = TRUE;
		}

		if(temp_flag_bus1 && temp_flag_bus2)
		{
			temp_flag_bus1 = FALSE;
			temp_flag_bus2 = FALSE;
			// 0 is OK, 1 is >45C, 2 is >60C, 3 is no them, 4 is ref err
			temp_status = adc_temp_check();

	      	if((temp_status != 0x00) && check_TEMP)
	      	{
	      			if (bpsMODE == SELFCHECK)
	      			{
	      				selfcheck_temp_error = TRUE;
	      			}
	      			else
	      			{
	      				if((current < 0.0) && (temp_status==0x01)) // charging & 45C
	      				{
	      					batt_KILL = TRUE;   // Temperature Error
	      					batt_ERR = 0x60;	// MAX temperature charge
	      				}
//	      				if((current >= 0.0) && (temp_status==0x02)) // discharge & 60C
	      				if(temp_status==0x02) // discharge & 60C
	      				{
	      					batt_KILL = TRUE;   // Temperature Error
	      					batt_ERR = 0x50;	// MAX temperature discharge
	      				}
	      				if(temp_status>=0x03) // thermistor not connected or 2.5 V ref error
	      				{
	      					batt_KILL = TRUE;   // Temperature Error
	      					batt_ERR = 0x70; 	// Broken Temp Sensor
	      				}

	      			}

	      	}
	      	else
	      	{
	      		selfcheck_temp_error = FALSE;
	      	}
		}

		if (cm_flag)
		{
			cm_flag = FALSE;

			//get current direction across batt shunt
/*			for(i = 1; i > 0; i--)
			{
				diff_ref = adc_misc_read_convert(1);						//check multiple times to avoid invalid data
				diff_shunt = adc_misc_read_convert(4);						//check multiple times to avoid invalid data
			}
*/
			diff_ref = adc_voltage[49];
			diff_shunt =  adc_voltage[52];

			current_dvolt = diff_shunt - diff_ref;

			current = (float)(current_dvolt) * CURRENT_I_SCALE;
			current /= CURRENT_FULL_SCALE;			// in mA

			//check temperature and current limits if discharging
			if((current >= 0) && check_CURRENT)								//adc > ref  (DISCHARGING)
			{
				if(current >= MAX_CURRENT_DISCHARGE)					//over current check
				{
					batt_KILL = TRUE;
					batt_ERR = 0x30; 			// Max Current Discharge
				}
			}

			//check temperature and current limitsbattery current if charging
			else if((current < 0) && check_CURRENT)									//adc < ref (CHARGING)
			{
				if(current <= MAX_CURRENT_CHARGE)						//over current check
				{
					batt_KILL = TRUE;
					batt_ERR = 0x40;		// Max Current Charge

				}
			}
		}

	   ////////////////////////////////////////////////////////////////////////////////////////////////

		/////////////////////////////////////////HANDLE CAN//////////////////////////////////////////

		// Handle communications events
		if (cancomm_flag && send_can)
		{
			cancomm_flag = FALSE;

		// Max Cell Voltage and total battery voltage
			max_v_cell = 0;
			ltc_max = 0x0000;
			bat_voltage = 0x00000000;
			for(i=0;i<=10;i++)
			{
				if((ltc1_cv[i])>ltc_max)
				{
					ltc_max=ltc1_cv[i];
					max_v_cell = i;
				}
				bat_voltage += (long) (ltc1_cv[i]-512);
			}
			for(i=0;i<=11;i++)
			{
				if((ltc2_cv[i])>ltc_max)
				{
					ltc_max=ltc2_cv[i];
					max_v_cell = i+11;
				}
				bat_voltage += (long) (ltc2_cv[i]-512);
			}
			for(i=0;i<=11;i++)
			{
				if((ltc3_cv[i])>ltc_max)
				{
					ltc_max=ltc3_cv[i];
					max_v_cell = i+23;
				}
				bat_voltage += (long) (ltc3_cv[i]-512);
			}

			max_voltage = ((float) (ltc_max-512)) * 0.0015;

		// Min Cell Voltage
			min_v_cell = 0;
			ltc_min = 0x0FFF;
			for(i=0;i<=10;i++)
			{
				if((ltc1_cv[i])<ltc_min)
				{
					ltc_min=ltc1_cv[i];
					min_v_cell = i;
				}
			}
			for(i=0;i<=11;i++)
			{
				if((ltc2_cv[i])<ltc_min)
				{
					ltc_min=ltc2_cv[i];
					min_v_cell = i+11;
				}
			}
			for(i=0;i<=11;i++)
			{
				if((ltc3_cv[i])<ltc_min)
				{
					ltc_min=ltc3_cv[i];
					min_v_cell = i+23;
				}
			}

			min_voltage = ((float) (ltc_min-512)) * 0.0015;

		// Max Cell Temperature Already Computed
			temp = ((float) max_temp_val / 16777216.0); // voltage ratio
			max_temp = 126.1575-311.329*temp;  // Linear Est. of Temp 20 - 45

		// Transmit CAN message
		// Transmit Max Cell Voltage
			can.address = BP_CAN_BASE + BP_VMAX;
			can.data.data_fp[1] = max_voltage;
			can.data.data_fp[0] = (float) max_v_cell;
			can_transmit();

		// Transmit Min Cell Voltage
			can.address = BP_CAN_BASE + BP_VMIN;
			can.data.data_fp[1] = min_voltage;
			can.data.data_fp[0] = (float) min_v_cell;
			can_transmit();

		// Transmit Max Cell Temperature
			can.address = BP_CAN_BASE + BP_TMAX;
			can.data.data_fp[1] = max_temp;
			can.data.data_fp[0] = (float) max_temp_idx;
			can_transmit();

		// Transmit Shunt Cutrrent
			can.address = BP_CAN_BASE + BP_ISH;
			can.data.data_fp[1] = current;
			can.data.data_fp[0] = (float) max_voltage * 0.0015;
			can_transmit();

		// Transmit our ID frame at a slower rate (every 10 events = 1/second)
			comms_event_count++;
			if(comms_event_count >=10)
			{
				comms_event_count = 0;
				can.address = BP_CAN_BASE;
				can.data.data_u8[7] = 'B';
				can.data.data_u8[6] = 'P';
				can.data.data_u8[5] = 'v';
				can.data.data_u8[4] = '1';
				can.data.data_u32[0] = DEVICE_SERIAL;
				cancheck_flag = can_transmit();
				if(cancheck_flag == 1) can_init();
			}

			can_flag_check();
		}  // End periodic communications

		  // Check for CAN packet reception
		if(((P1IN & CAN_INTn) == 0x00) && send_can)
		{
		// IRQ flag is set, so run the receive routine to either get the message, or the error
			can_receive();
		// Check the status
		// Modification: case based updating of actual current and velocity added
		// - messages received at 5 times per second 16/(2*5) = 1.6 sec smoothing
			if(can.status == CAN_OK)
			{
				LED_ERROR_OFF;
				switch(can.address)
				{
				case DC_CAN_BASE + DC_SWITCH:
					switches_out_dif  = can.data.data_u16[3];
					switches_dif_save = can.data.data_u16[2];
					switches_out_new  = can.data.data_u16[1];
					switches_new      = can.data.data_u16[0];
					if((switches_new & SW_IGN_ON) == 0x00)
					{
						if((switches_out_new & 0xFF00) == 0xFF00)
						{
							can_start_precharge = TRUE;
						}
					}
					else
					{
						can_car_enable = TRUE;
					}
					if((switches_new & SW_IGN_ACC) == 0x00)
					{
						if((switches_out_new & 0xFF00) == 0x0F00)
						{
							dc_charge_mode = TRUE;
						}
					}
					else
					{
						dc_charge_mode = FALSE;
					}
					// Add DC Charge mode here
					can_DC_rcv_count++;
					break;
				case AC_CAN_BASE + AC_BP_CHARGE:
					AC_char3 = can.data.data_u8[7];
					AC_char2 = can.data.data_u8[6];
					AC_char1 = can.data.data_u8[5];
					AC_char0 = can.data.data_u8[4];
					AC_Serial_No = can.data.data_u32[0];
					// If ACV1 charge mode, else 0x0000
					if((AC_char3 == 'A') && (AC_char2 == 'C') && (AC_char1 == 'v') && (AC_char0 == '1'))
					{
						ac_charge_mode = TRUE;
					}
					else
					{
						ac_charge_mode = FALSE;
					}

					break;
				default:
					break;
				}

			}
			else if(can.status == CAN_RTR)
			{
				LED_ERROR_OFF;
				switch(can.address)
				{
					case BP_CAN_BASE:
						can.address = BP_CAN_BASE;
						can.data.data_u8[7] = 'B';
						can.data.data_u8[6] = 'P';
						can.data.data_u8[5] = 'v';
						can.data.data_u8[4] = '1';
						can.data.data_u32[0] = DEVICE_SERIAL;
						can_transmit();
						break;
					case BP_CAN_BASE + BP_VMAX:
						can.data.data_fp[1] = max_voltage;
						can.data.data_fp[0] = (float) max_v_cell;
						can_transmit();
						break;
					case BP_CAN_BASE + BP_VMIN:
						can.data.data_fp[1] = min_voltage;
						can.data.data_fp[0] = (float) min_v_cell;
						can_transmit();
						break;
					case BP_CAN_BASE + BP_TMAX:
						can.data.data_fp[1] = max_temp;
						can.data.data_fp[0] = (float) max_temp_idx;
						can_transmit();
						break;
					case BP_CAN_BASE + BP_ISH:
						can.data.data_fp[1] = current;
						can.data.data_fp[0] = (float) max_voltage * 0.0015;
						can_transmit();
						break;
					case BP_CAN_BASE + BP_PCDONE:
						if(bpsMODE == NORMALOP)
						{
							can.data.data_u8[7] = 'B';
							can.data.data_u8[6] = 'P';
							can.data.data_u8[5] = 'v';
							can.data.data_u8[4] = '1';
							can.data.data_u32[0] = DEVICE_SERIAL;
						}
						else
						{
							can.data.data_u8[7] = 'B';
							can.data.data_u8[6] = 'P';
							can.data.data_u8[5] = 'N';
							can.data.data_u8[4] = 'P';
							can.data.data_u32[0] = DEVICE_SERIAL;
						}
						can_transmit();
						break;
				}
			}
			else if(can.status == CAN_ERROR)
			{
				LED_ERROR_TOG;
				can_err_cnt++;
			}
		}

		/////////////////////////////////////////HANDLE RS232//////////////////////////////////////////
		// ADC data locations
		// Temp Locations: 1,2,3,4,5,6,7,  9,10,11,12,13,14,15,  17,18,19,20,21,22,23,
		// Temp Locations: 25,26,27,28,29,30,31  33,34,35,36,37,38,39, 46,47
		// Temp Locations: 50, 51

		// 2.5V Locations: 0,8,16,24,32,40,48
		// MC PC Locations: 0,8,16,24,32,40,48
		// SCAP Ref & Shunt Location: 45 & 41
		// SCAP PC Locations: 42, 43, 44
		// Bat Ref & Shunt Location: 49 & 52
		// MC PC Locations: 53, 54, 55



		if(batt_temp_status)	//cell temperatures
		{
			batt_temp_status = 0;
			BPS2PC_puts("\nBATTERY TEMPERATURES:");
			BPS2PC_puts("MAX Discharge Temp = 60 Degree C");
			BPS2PC_puts("MAX Charge Temp = 45 Degree C\n");

			for(i = 1; i < 48; i++)
			{
				volt = ((float) adc_voltage[i] / 16777216.0); // voltage ratio
				temp = 126.1575-311.329*volt;  // Linear Est. of Temp 20 - 45
				temp1 = (int)temp;
				temp2 = (int)(temp * 10) % 10;
				temp3 = (int)(temp * 100) %10;

				sprintf(buff,"Temp %d = %d.%d%d Degree C",i,temp1,temp2,temp3);
				BPS2PC_puts(buff);
			}

			temp = max_temp;
			temp1 = (int)temp;
			temp2 = (int)(temp * 10) % 10;
			temp3 = (int)(temp * 100) %10;
			sprintf(buff,"Max Temp %d = %d.%d%d Degree C",max_temp_idx,temp1,temp2,temp3);
			BPS2PC_puts(buff);

		}

		else if(batt_volt_status)										//cell voltages
		{

			batt_volt_status = 0;
			BPS2PC_puts("\nBATTERY CELL VOLTAGES:");
			BPS2PC_puts("MAX CELL VOLTAGE 4.176 V");
			BPS2PC_puts("MIN CELL VOLTAGE 2.640 V\n");

			for(i = 0; i < 11; i++)
			{
				temp = (float) (ltc1_cv[i]-512) / 666.66;								//conversion to volts
				temp1 = (int)temp;										//integer value
				temp2 = (int)(temp * 10) % 10;							//tenths place
				temp3 = (int)(temp * 100) %10;							//hundredths place

				sprintf(buff, "Cell %d = %d.%d%d Volts",i,temp1,temp2,temp3);
				BPS2PC_puts(buff);
			}
			for(i = 0; i < 12; i++)
			{
				temp = (float)(ltc2_cv[i]-512) / 666.66;
				temp1 = (int)temp;
				temp2 = (int)(temp * 10) % 10;
				temp3 = (int)(temp * 100) %10;

				sprintf(buff, "Cell %d = %d.%d%d Volts",i+11,temp1,temp2,temp3);
				BPS2PC_puts(buff);
			}
			for(i = 0; i < 12; i++)
			{
				temp = (float) (ltc3_cv[i]-512) / 666.66;
				temp1 = (int)temp;
				temp2 = (int)(temp * 10) % 10;
				temp3 = (int)(temp * 100) %10;

				sprintf(buff, "Cell %d = %d.%d%d Volts",i+23,temp1,temp2,temp3);
				BPS2PC_puts(buff);
			}

			temp = (float) (bat_voltage) / 666.66;
			temp1 = (int)temp;
			temp2 = (int)(temp * 10) % 10;
			temp3 = (int)(temp * 100) %10;
			sprintf(buff, "Battery = %d.%d%d Volts",temp1,temp2,temp3);
			BPS2PC_puts(buff);
		}

		else if(batt_current_status)								//battery current
		{
			batt_current_status = 0;

			BPS2PC_puts("\nBATTERY CURRENT:");
			BPS2PC_puts("MAX CURRENT DISCHARGE 80200 mA");
			BPS2PC_puts("MAX CURRENT CHARGE   -19500 mA\n");

			temp = current;
			temp1 = (int)temp;
			temp2 = abs((int)(temp * 10) % 10);
			temp3 = abs((int)(temp * 100) %10);

			sprintf(buff, "Battery Current = %d.%d%d mA",temp1,temp2,temp3);
			BPS2PC_puts(buff);
		}
		else if(batt_state_status)								//battery current
		{
			batt_state_status = 0;

			BPS2PC_puts("\nBATTERY STATE:");

			sprintf(buff, "Battery State = %d",bpsMODE+1);
			BPS2PC_puts(buff);
		}

		///////////////////////////////////////////////////////////////////////////////////////////////////////////////

		// BUTTON1 Press Received   
		if(((P1IN & BUTTON1) != BUTTON1) || ((int_op1_flag & 0x08) == 0x08))
		{
			int_op1_flag &= 0x08;
		    
			//open all relays
			relay_mcpc_open;
			ext_relay_mcpc_open;
			relay_mc_open;

			bps_strobe_off;

		   	bpsMODE = CHARGE;
		   	charge_mode |= 0x10;
		    mode_count = 0;
		    mode_dwell_count = 0;
			P6OUT &= ~(LED5);		//DR LED 0x8
			P6OUT |=  (LED4|LED3|LED2);		//

		}

		// BUTTON2 Press Received   
		if(((P1IN & BUTTON2) != BUTTON2) || ((int_op1_flag & 0x04) == 0x04))
		{
			int_op1_flag &= 0x04;

		    //open all relays
			relay_mcpc_open;
			ext_relay_mcpc_open;
			relay_mc_open;
			delay();
			relay_batt_open;
			delay();
			relay_array_open;

			bps_strobe_off;

		    bpsMODE = INITIALIZE;
			P6OUT |= (LED2|LED3|LED4|LED5);
		    DR_LED0_ON;
		    mode_count = 0;
		    mode_dwell_count = 0;


		}

		//handle batt_KILL error

		if(batt_KILL)
		{
			//open all relays
			relay_mcpc_open;
			ext_relay_mcpc_open;
			relay_mc_open;
			delay();
			relay_batt_open;
			delay();
			relay_array_open;

			if(bpsMODE != ERRORMODE)
			{
				// Set Indicators
				LED_ERROR_ON;
				LED_NORMALOP_OFF;
				bps_strobe_on;
				capture_err = (batt_ERR>>4) & 0x0F;
				P6OUT |= 0x0F;
				P6OUT &= ~(capture_err);

				PC_Complete = FALSE;
				MC_Complete = FALSE;

				send_can = FALSE;
			}
			batt_KILL = FALSE;

			bpsMODE = ERRORMODE;
		    mode_count = 0;
		    mode_dwell_count = 0;

		}
		else
		{
			LED_ERROR_OFF;
		}
		WDTCTL = WDT_ARST_1000; // Stop watchdog timer to prevent time out reset

	} //end while(TRUE)

}
/*================================ ** END MAIN** =========================================*/
////////////////////////////////////////////////////////////////////////////////////////////

/*
* Initialise Timer B
*	- Provides timer tick timebase at 100 Hz
*/
void timerB_init( void )
{
  TBCTL = CNTL_0 | TBSSEL_1 | ID_3 | TBCLR;		// ACLK/8, clear TBR
  TBCCR0 = (ACLK_RATE/8/TICK_RATE);				// Set timer to count to this value = TICK_RATE overflow
  TBCCTL0 = CCIE;								// Enable CCR0 interrrupt
  TBCTL |= MC_1;								// Set timer to 'up' count mode
}

/*
* Timer B CCR0 Interrupt Service Routine
*	- Interrupts on Timer B CCR0 match at 10Hz
*	- Sets Time_Flag variable
*/
/*
* GNU interropt symantics
* interrupt(TIMERB0_VECTOR) timer_b0(void)
*/
#pragma vector = TIMERB0_VECTOR
__interrupt void timer_b0(void)
{
	static unsigned int status_count = LTC_STATUS_COUNT;
	static unsigned int cancomm_count = CAN_COMMS_COUNT;

	// Primary System Heartbeat
	status_count--;
    if( status_count == 0 )
    {
    	status_count = LTC_STATUS_COUNT;
    	status_flag = TRUE;
    }

	// Periodic CAN Satus Transmission
    if(send_can) cancomm_count--;
    if( cancomm_count == 0 )
    {
        cancomm_count = CAN_COMMS_COUNT;
        cancomm_flag = TRUE;
    }
}

//RS232 Interrupt
#pragma vector = USCI_A3_VECTOR
__interrupt void USCI_A3_ISR(void)
{
	UCA3IV = 0x00;

	command[rs232_count] = BPS2PC_getchar();		//get character
	BPS2PC_putchar(command[rs232_count]);			//put character

	if(command[rs232_count] == 0x0D)				//if return
	{
		rs232_count = 0x00;							//reset counter
		if(strcmp(batt_temp,command) == 0) batt_temp_status = 1;
		else if(strcmp(batt_volt,command) == 0) batt_volt_status = 1;
		else if(strcmp(batt_current, command) == 0) batt_current_status = 1;
		else if(strcmp(batt_state, command) == 0) batt_state_status = 1;
		else
		{
			batt_temp_status = 0;
			batt_volt_status = 0;
			batt_current_status = 0;
			batt_state_status = 0;
		}

		for(i = 0; i < 31; i++)
		{
			command[i] = 0;							//reset command
		}
		BPS2PC_putchar(0x0A);						//new line
		BPS2PC_putchar(0x0D);						//return
	}
	else if(command[rs232_count] != 0x7F)			//if not backspace
	{
		rs232_count++;
	}
	else
	{
		rs232_count--;
	}

}

/*
* Port 1 Pin Interrupt Service Routine
*/
	#pragma vector=PORT1_VECTOR
__interrupt void P1_ISR(void)
{
  switch(__even_in_range(P1IV,16))
  {
  case 0:break;                             // Vector 0 - no interrupt
  case 2:                                   // Vector 1.0 -
    break;
  case 4:                                   // Vector 1.1 -
    break;
  case 6:                                   // Vector 1.2 - BUTTON1
    int_op1_flag |= 0x08;
    break;
  case 8:                                   // Vector 1.3 - BUTTON2
    int_op1_flag |= 0x04;
   break;
  case 10:                                  // Vector 1.4 -
    break;
  case 12:                                  // Vector 1.5 - CAN_RX0n
    int_op1_flag |= 0x20;
    break;
  case 14:                                  // Vector 1.6 - CAN_RX1n
    int_op1_flag |= 0x40;
    break;
  case 16:                                  // Vector 1.7 -
    break;
  default:
    break;
  }
}

/*
 * Port 2 Pin Interrupt Service Routine
 */
	#pragma vector=PORT2_VECTOR
__interrupt void P2_ISR(void)
{
  switch(__even_in_range(P2IV,16))
  {
  case 0:break;                             // Vector 0 - no interrupt
  case 2:                                   // Vector 2.0 - CAN_INTn
	  int_op2_flag |= 0x01;
    break;
  case 4:                                   // Vector 2.1 - ADC1_RDYn
	  adc_rdy_flag |= 0x02;
    break;
  case 6:                                   // Vector 2.2 - ADC2_RDYn
	  adc_rdy_flag |= 0x04;
    break;
  case 8:                                   // Vector 2.3 - ADC3_RDYn
	  adc_rdy_flag |= 0x08;
    break;
  case 10:                                  // Vector 2.4 - ADC4_RDYn
	  adc_rdy_flag |= 0x10;
    break;
  case 12:                                  // Vector 2.5 - ADC5_RDYn
	  adc_rdy_flag |= 0x20;
    break;
  case 14:                                  // Vector 2.6 - ADC6_RDYn
	  adc_rdy_flag |= 0x40;
    break;
  case 16:                                  // Vector 2.7 - ADC7_RDYn
	  adc_rdy_flag |= 0x80;
    break;
  default:
    break;
  }
}
