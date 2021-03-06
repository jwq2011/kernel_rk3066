/* This Lcd Driver is HSD070IDW1 write by cst 2009.10.27 */

#ifndef __LCD_AUO__
#define __LCD_AUO__




/* Base */
#define SCREEN_TYPE		SCREEN_RGB
#define LVDS_FORMAT      	LVDS_8BIT_1
#define OUT_FACE		OUT_P888//OUT_P888///OUT_P565   /////OUT_P666
///#define DCLK			40000000
#define DCLK			52000000   ////33000000
#define LCDC_ACLK       	150000000 ///500000000     //29 lcdc axi DMA Ƶ��

/* Timing */
#define H_PW			3///1//30//48 //10
#define H_BP			50///20///50//46//10//40 //100
#define H_FP			30///60///30///210// //210
#define H_VD			1024///800 //1024

#define V_PW		1//3//	  2//        3//13//10
#define V_BP		35//107//yl107//qw35///23//	 18     //    23//10// //10
#define V_FP		20//1//yl1//qw20///2//	8//           12//22 //18
#define V_VD			480 //768
/* Other */
#define DCLK_POL               1
#define SWAP_RB			0
#define DEN_POL		0
#define VSYNC_POL	0
#define HSYNC_POL	0

#define SWAP_RB		0
#define SWAP_RG		0
#define SWAP_GB		0


#define LCD_WIDTH       162//154    //need modify
#define LCD_HEIGHT      121//85
#endif

