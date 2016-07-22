/* SRanger and Gxsm - Gnome X Scanning Microscopy Project
 * universal STM/AFM/SARLS/SPALEED/... controlling and
 * data analysis software
 *
 * DSP tools for Linux
 *
 * Copyright (C) 1999,2000,2001,2002 Percy Zahl
 *
 * Authors: Percy Zahl <zahl@users.sf.net>
 * WWW Home:
 * DSP part:  http://sranger.sf.net
 * Gxsm part: http://gxsm.sf.net
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA.
 */

/* -*- Mode: C++; indent-tabs-mode: nil; c-basic-offset: 8 c-style: "K&R" -*- */

#include "FB_spm_dataexchange.h"
#include "dataprocess.h"

/* compensate for X AIC pipe delay and add user "nx_pre" amount */
#define PIPE_LEN (5+scan.nx_pre)

#define Z_DATA_BUFFER_SIZE    0x2000
#define Z_DATA_BUFFER_MASK    0x3fff

#define EN_PROBE_TRIGGER      0x0008

extern SPM_PI_FEEDBACK  feedback;
extern FEEDBACK_MIXER   feedback_mixer;
extern ANALOG_VALUES    analog;
extern AREA_SCAN        scan;
extern PROBE            probe;
extern DATA_FIFO        datafifo;
extern DATA_FIFO        probe_datafifo;
extern CR_GENERIC_IO    CR_generic_io;
extern SCAN_EVENT_TRIGGER scan_event_trigger;
#ifdef WATCH_ENABLE
extern WATCH            watch;
#endif

/* THIS IS DOUBLE USED !!! NO PROBE WHILE USED OF Z_DATA_BUFFER !!! */
extern DSP_INT      prbdf;

#define PRB_SE_DELAY       8        /* Start/End Delay -- must match !!*/

int AS_ip, AS_jp, AS_kp, AS_mod;
int AS_ch2nd_scan_switch; /* const.height 2nd scan mode flag X+/-, holds next mode to switch to 2nd scanline */
int AS_ch2nd_constheight_enabled; /* const H mode flg */

long AS_AIC_data_sum[9];
int  AS_AIC_num_samples;

#define BZ_MAX_CHANNELS 16
long bz_last[BZ_MAX_CHANNELS];
int  bz_byte_pos;

#define BZ_PUSH_NORMAL   0x00000000UL // normal atomatic mode using size indicator bits 31,30:
// -- Info: THIS CONST NAMES .._32,08,16,24 ARE NOT USED, JUST FOR DOCUMENMTATION PURPOSE
#define BZ_PUSH_MODE_32  0x00000000UL // 40:32 => 0b00MMMMMM(8bit) 0xDDDDDDDD(32bit), M:mode bits, D:Data
#define BZ_PUSH_MODE_08  0xC0000000UL //  8:6  => 0b11DDDDDD
#define BZ_PUSH_MODE_16  0x80000000UL // 16:14 => 0b10DDDDDD DDDDDDDD
#define BZ_PUSH_MODE_24  0x40000000UL // 24:22 => 0b01DDDDDD DDDDDDDD DDDDDDDD
#define BZ_PUSH_MODE_32_START   0x01000000UL // if 31,30=0 bits 29..0 available for special modes: 0x02..0x3f
// more modes free for later options and expansions
#define BZ_PUSH_MODE_32_FINISH  0x3f000000UL // finish up with Full Zero 32 bit record of max length as AS-END mark.
DSP_ULONG bz_push_mode;

short *Z_data_buffer;

/* Scan Control States */
#define AS_READY    0
#define AS_MOVE_XY  1
#define AS_SCAN_XP  2
#define AS_SCAN_YP  3
#define AS_SCAN_XM  4
#define AS_SCAN_YM  5
#define AS_SCAN_2ND_XP  6
#define AS_SCAN_2ND_XM  7

void bz_init(){ 
	int i; 
	for (i=0; i<(DATAFIFO_LENGTH>>1); ++i) datafifo.buffer.ul[i] = 0x3f3f3f3fUL;
	for (i=0; i<BZ_MAX_CHANNELS; ++i) bz_last[i] = 0L; 
	bz_push_mode = BZ_PUSH_MODE_32_START; 
	bz_byte_pos=0;
}

void bz_push(int i, long x){
	union { DSP_LONG l; DSP_ULONG ul; } delta;
	int bits;
	delta.l = _lssub (x, bz_last[i]); // if this saturates, original x is packed in 32bit mode either way.
	bz_last[i] = x;

	if (bz_push_mode) 
		goto bz_push_mode_32;

	bits = delta.l != 0 ? _lnorm (delta.l) : 32;

//	std::cout << "BZ_PUSH(i=" << i << ", ***x=" << std::hex << x << std::dec << ") D.l=" << delta.l << " bits=" << bits;
	if (bits > 26){ // 8 bit package, 6 bit data
		delta.l <<= 26;
//		std::cout << ">26** 8:6bit** BZdelta.l=" << std::hex << delta.l << std::dec << " bz_byte_pos=" << bz_byte_pos << std::endl;;
		switch (bz_byte_pos){
		case 0:  datafifo.buffer.ul[datafifo.w_position>>1]  =  0xC0000000UL | (delta.ul>>2)       ; bz_byte_pos=1; break;
		case 1:  datafifo.buffer.ul[datafifo.w_position>>1] |= (0xC0000000UL | (delta.ul>>2)) >>  8; bz_byte_pos=2; break;
		case 2:  datafifo.buffer.ul[datafifo.w_position>>1] |= (0xC0000000UL | (delta.ul>>2)) >> 16; bz_byte_pos=3; break;
		default: datafifo.buffer.ul[datafifo.w_position>>1] |= (0xC0000000UL | (delta.ul>>2)) >> 24; bz_byte_pos=0;
			datafifo.w_position += 2;
			datafifo.w_position &= DATAFIFO_MASK;
			break;
		}
	} else if (bits > 18){ // 16 bit package, 14 bit data
		delta.l <<= 18;
//		std::cout << ">18**16:14bit** BZdelta.l=" << std::hex << delta.l << std::dec << " bz_byte_pos=" << bz_byte_pos << std::endl;;
 		switch (bz_byte_pos){
		case 0:  datafifo.buffer.ul[datafifo.w_position>>1]  =  0x80000000UL | (delta.ul>>2)       ; bz_byte_pos=2; break;
		case 1:  datafifo.buffer.ul[datafifo.w_position>>1] |= (0x80000000UL | (delta.ul>>2)) >>  8; bz_byte_pos=3; break;
		case 2:  datafifo.buffer.ul[datafifo.w_position>>1] |= (0x80000000UL | (delta.ul>>2)) >> 16; bz_byte_pos=0;
			datafifo.w_position += 2;
			datafifo.w_position &= DATAFIFO_MASK;
			break;
		default: datafifo.buffer.ul[datafifo.w_position>>1] |= (0x80000000UL | (delta.ul>>2)) >> 24; 
			datafifo.w_position += 2;
			datafifo.w_position &= DATAFIFO_MASK;
			datafifo.buffer.ul[datafifo.w_position>>1] = ((delta.ul>>2)&0x00ff0000UL) << 8; bz_byte_pos=1; break;
		}
	} else if (bits > 10){ // 24 bit package, 22 bit data
		delta.l <<= 10;
//		std::cout << ">10**24:22bit** BZdelta.l=" << std::hex << delta.l << std::dec << " bz_byte_pos=" << bz_byte_pos << std::endl;
		switch (bz_byte_pos){
		case 0:  datafifo.buffer.ul[datafifo.w_position>>1]  =  0x40000000UL | (delta.ul>>2)       ; bz_byte_pos=3; break;
		case 1:  datafifo.buffer.ul[datafifo.w_position>>1] |= (0x40000000UL | (delta.ul>>2)) >>  8; bz_byte_pos=0;
			datafifo.w_position += 2;
			datafifo.w_position &= DATAFIFO_MASK;
			break;
		case 2:  datafifo.buffer.ul[datafifo.w_position>>1] |= (0x40000000UL | (delta.ul>>2)) >> 16;
			datafifo.w_position += 2;
			datafifo.w_position &= DATAFIFO_MASK;
			datafifo.buffer.ul[datafifo.w_position>>1] = ((delta.ul>>2)&0x0000ff00UL) << 16; bz_byte_pos=1; break;
		default: datafifo.buffer.ul[datafifo.w_position>>1] |= (0x40000000UL | (delta.ul>>2)) >> 24;
			datafifo.w_position += 2;
			datafifo.w_position &= DATAFIFO_MASK;
			datafifo.buffer.ul[datafifo.w_position>>1] = ((delta.ul>>2)&0x00ffff00UL) << 8; bz_byte_pos=2; break;
		}
	} else { // full 32 bit data -- special on byte alignment -- 6 spare unused bits left!
	bz_push_mode_32:
		delta.l = x;
//		std::cout << "ELSE**40:32bit** BZdelta.l=" << std::hex << delta.l << std::dec << " bz_byte_pos=" << bz_byte_pos << std::endl;
//		bits=datafifo.w_position>>1;

		switch (bz_byte_pos){
		case 0:  datafifo.buffer.ul[datafifo.w_position>>1]  =  bz_push_mode | (delta.ul >> 8); // 0xMMffffff..
			datafifo.w_position += 2;
			datafifo.w_position &= DATAFIFO_MASK;
			datafifo.buffer.ul[datafifo.w_position>>1] = delta.ul << 24; bz_byte_pos=1; break;
		case 1:  datafifo.buffer.ul[datafifo.w_position>>1] |= (bz_push_mode | (delta.ul >> 8)) >> 8; // 0x==MMffff....
			datafifo.w_position += 2;
			datafifo.w_position &= DATAFIFO_MASK;
			datafifo.buffer.ul[datafifo.w_position>>1] = delta.ul << 16; bz_byte_pos=2; break;
		case 2:  datafifo.buffer.ul[datafifo.w_position>>1] |= (bz_push_mode | (delta.ul >> 8)) >> 16; // 0x====MMff......
			datafifo.w_position += 2;
			datafifo.w_position &= DATAFIFO_MASK;
			datafifo.buffer.ul[datafifo.w_position>>1] = delta.ul <<  8; bz_byte_pos=3; break;
		default: datafifo.buffer.ul[datafifo.w_position>>1] |= bz_push_mode >> 24; // 0x====MM........
			datafifo.w_position += 2;
			datafifo.w_position &= DATAFIFO_MASK;
			datafifo.buffer.ul[datafifo.w_position>>1] = delta.ul;
			datafifo.w_position += 2;
			datafifo.w_position &= DATAFIFO_MASK; bz_byte_pos=0;
			break;
		}
	}
//		Xdumpbuffer ((unsigned char*)&datafifo.buffer.ul[bits-4], 32, 0);
}

void clear_summing_data (){
	AS_AIC_data_sum[0] = AS_AIC_data_sum[1] = AS_AIC_data_sum[2] =
	AS_AIC_data_sum[3] = AS_AIC_data_sum[4] = AS_AIC_data_sum[5] =
	AS_AIC_data_sum[6] = AS_AIC_data_sum[7] = AS_AIC_data_sum[8] = 0L;
	analog.counter[0] = 0L;
	AS_AIC_num_samples = 0;
}

/* calc of f_dx/y and num_steps by host! */
void init_area_scan (){
	// wait until fifo read thread on host is ready
	if (!datafifo.stall) {
		// reset FIFO -- done by host

		init_probe_fifo (); // reset probe fifo once at start!

		// now starting...
		scan.start = 0;
		scan.stop  = 0;
				
		// calculate num steps to go to starting point -- done by host

		// init probe trigger count (-1 will disable it)
		AS_mod = scan.dnx_probe;
		if (AS_mod > 0){
			AS_ip = 1;
			AS_jp = 0;
			AS_kp = 0;
		} else {
			AS_jp = -1;
		}
		AS_ch2nd_scan_switch = AS_SCAN_YP; // just go to next line by default
		AS_ch2nd_constheight_enabled = 0;  // disable first
					
		// 2nd stage line scan, memorized Z OK -- here we have a size limit due to the buffer used! ?
		if (scan.srcs_2nd_xp || scan.srcs_2nd_xm)
			if (scan.nx < Z_DATA_BUFFER_SIZE)
				AS_ch2nd_scan_switch = AS_SCAN_2ND_XP; // enable 2nd scan line mode

		if ((scan.srcs_xp & 0x07000) || (scan.srcs_xm & 0x07000))  // setup LockIn job
			init_lockin_for_bgjob ();


		if ((scan.srcs_xp & 0x08000) || (scan.srcs_xm & 0x08000)){ // if Counter channel requested, restart counter/timer now
			analog.counter[0] = 0L;
			analog.counter[1] = 0L;
		}

		bz_init();

		Z_data_buffer = &prbdf; // using probe buffer -- shared!

		// enable subtask
		scan.sstate = AS_MOVE_XY;
		scan.pflg  = 1;
	}
}

void finish_area_scan (){
	int i;
	bz_push_mode = BZ_PUSH_MODE_32_FINISH;
	for (i=0; i<BZ_MAX_CHANNELS; ++i)
		bz_push (i, 0);
	scan.sstate = AS_READY;
	scan.pflg = 0;
}

void integrate_as_data_srcs (long srcs){
	if (srcs & 0x01) // PIDSrcA1 (Dest) --> Zmonitor (Topo) feedback generated Z signal
		AS_AIC_data_sum[8] += -feedback.z;

	if (srcs & 0x0010) // DataSrcA1 --> AIC0 <-- I (current), ...
		AS_AIC_data_sum[0] += AIC_IN(0);

	if (srcs & 0x0020) // DataSrcA2 --> AIC1
		AS_AIC_data_sum[1] += AIC_IN(1);

	if (srcs & 0x0040) // DataSrcA3 --> AIC2
		AS_AIC_data_sum[2] += AIC_IN(2);

	if (srcs & 0x0080) // DataSrcA4 --> AIC3
		AS_AIC_data_sum[3] += AIC_IN(3);

	if (srcs & 0x0100) // DataSrcB1 --> AIC4
		AS_AIC_data_sum[4] += AIC_IN(4);

	if (srcs & 0x0200) // DataSrcB2 --> AIC5
		AS_AIC_data_sum[5] += AIC_IN(5);

	if (srcs & 0x0400) // DataSrcB3 --> AIC6
		AS_AIC_data_sum[6] += AIC_IN(6);

	if (srcs & 0x0800) // DataSrcB4 --> AIC7
		AS_AIC_data_sum[7] += AIC_IN(7);

	++AS_AIC_num_samples;
}

void push_area_scan_data (unsigned long srcs){
	DSP_ULONG tmp;

	// read and buffer (for Rate Meter, gatetime not observed -- always last completed count)
	CR_generic_io.count_0 = analog.counter[0];

	if (srcs & 0xFFF1){
		// using bz_push for bit packing and delta signal usage
		// init/reinit data set?

		tmp = (srcs << 16) | AS_AIC_num_samples;
		if (tmp != bz_last[0]){ // re init using BZ_PUSH_MODE_32_START to indicate extra info set
			bz_push_mode = BZ_PUSH_MODE_32_START; 
			bz_push (0, tmp);
			bz_push_mode = BZ_PUSH_NORMAL;
		}

		if (srcs & 0x0001) // PIDSrc1/Dest <-- Z = -AIC_Z-value ** AS_AIC_data_sum[8]/AS_AIC_num_samples
			bz_push (1, feedback.z32);
		if (srcs & 0x0010) // DataSrcA1 --> AIC0
			bz_push (2, AS_AIC_data_sum[0]);
		if (srcs & 0x0020) // DataSrcA2 --> AIC1
			bz_push (3, AS_AIC_data_sum[1]);
		if (srcs & 0x0040) // DataSrcA3 --> AIC2
			bz_push (4, AS_AIC_data_sum[2]);
		if (srcs & 0x0080) // DataSrcA4 --> AIC3
			bz_push (5, AS_AIC_data_sum[3]);
		if (srcs & 0x0100) // DataSrcB1 --> AIC4
			bz_push (6, AS_AIC_data_sum[4]);
		if (srcs & 0x0200) // DataSrcB2 --> AIC5
			bz_push (7, AS_AIC_data_sum[5]);
		if (srcs & 0x0400) // DataSrcB3 --> AIC6
			bz_push (8, AS_AIC_data_sum[6]);
		if (srcs & 0x0800) // DataSrcB4 --> AIC7
			bz_push (9, AS_AIC_data_sum[7]);

		if (srcs & 0x01000) // DataSrcC1 --> LockIn1stA
			bz_push (10, probe.LockIn_1stA);
		if (srcs & 0x02000) // DataSrcD1 --> LockIn2ndA
			bz_push (11, probe.LockIn_2ndA);
		if (srcs & 0x04000) // DataSrcE1 --> LockIn0
			bz_push (12, probe.LockIn_0); 
		if (srcs & 0x08000) // "DataSrcF1" last CR Counter count
			bz_push (13, analog.counter[0]);
	}

	// auto clear now including counter0
	clear_summing_data ();
	analog.counter[0] = 0L;
}

void check_scan_event_trigger (short trig_i[8], short bias_setpoint[8]){
	int i;
	for (i=0; i<8; ++i)
		if (scan.ix == trig_i[i]){
			if (i<4)
				analog.out[ANALOG_BIAS] = bias_setpoint [i];
			else
				feedback_mixer.setpoint[0] = bias_setpoint [i];
		}
}

void run_area_scan (){
	if ((scan.srcs_xp & 0x07000) || (scan.srcs_xm & 0x07000)){
		// run LockIn
		// probe.AC_ix = 0; // -PRB_SE_DELAY - 2;
		run_lockin ();
	}

	switch (scan.sstate){
	case AS_SCAN_XP: // "X+" -> scan and dataaq.
		if (scan.iix < scan.dnx)
			integrate_as_data_srcs (scan.srcs_xp);
//		scan.xyz_vec[i_X] += scan.fs_dx;
		scan.xyz_vec[i_X] = _lsadd (scan.xyz_vec[i_X], scan.cfs_dx); // this is with SAT!!
		if (!scan.iix--){
			if (scan.ix--){
				if (AS_ip >= 0 && (AS_jp == 0 || scan.raster_a) && (scan.srcs_xp &  EN_PROBE_TRIGGER)){
					if (! --AS_ip){ // trigger probing process ?
						if (!probe.pflg) // assure last prb job is done!!
							init_probe ();
						AS_ip = scan.dnx_probe;
					}
				}
				if (scan_event_trigger.pflg)
					check_scan_event_trigger (scan_event_trigger.trig_i_xp, scan_event_trigger.xp_bias_setpoint);

				scan.iix    = scan.dnx-1;
				
				if (AS_ch2nd_constheight_enabled){
					feedback.z = _sadd (Z_data_buffer [scan.ix & Z_DATA_BUFFER_MASK], scan.Zoff_2nd_xp); // use memorized ZA profile + offset
					push_area_scan_data (scan.srcs_2nd_xp); // get 2nd XP data here!
				}else{
					if (AS_ch2nd_scan_switch == AS_SCAN_2ND_XP) // only if enabled 2nd scan line mode
						Z_data_buffer [scan.ix & Z_DATA_BUFFER_MASK] = feedback.z; // memorize Z profile

					push_area_scan_data (scan.srcs_xp); // get XP data here!
				}
			}
			else{
				scan.ix  = scan.nx;
				scan.iix = scan.dnx + PIPE_LEN;
				if (AS_ch2nd_constheight_enabled)
					scan.sstate = AS_SCAN_2ND_XM;
				else
					scan.sstate = AS_SCAN_XM;
				clear_summing_data ();
			}
			scan.cfs_dx = scan.fs_dx;
		}
		break;

	case AS_SCAN_XM: // "X-" <- scan and dataaq.
		scan.ifr = scan.fast_return;
		do{ // fast rep this for return speedup option
			if (scan.iix < scan.dnx)
				integrate_as_data_srcs (scan.srcs_xm);
//			scan.xyz_vec[i_X] -= scan.fs_dx;
			scan.xyz_vec[i_X] = _lssub (scan.xyz_vec[i_X], scan.cfs_dx);
			if (!scan.iix--){
				if (scan.ix--){
					if (AS_ip >= 0 && AS_jp == 0 && (scan.srcs_xm & EN_PROBE_TRIGGER)){
						if (! --AS_ip){ // trigger probing process ?
							if (!probe.pflg) // assure last prb job is done!!
								init_probe ();
							AS_ip = scan.dnx_probe;
						}
					}
					if (scan_event_trigger.pflg)
						check_scan_event_trigger (scan_event_trigger.trig_i_xm, scan_event_trigger.xm_bias_setpoint);
					
					scan.iix   = scan.dnx-1;
					
					if (AS_ch2nd_constheight_enabled){
						feedback.z = _sadd (Z_data_buffer [(scan.nx-scan.ix-1) & Z_DATA_BUFFER_MASK], scan.Zoff_2nd_xm); // use memorized ZA profile + offset
						push_area_scan_data (scan.srcs_2nd_xm); // get 2nd XP data here!
					}else{
						push_area_scan_data (scan.srcs_xm); // get XM data here!
					}
				}
				else{
					if (!scan.iy){ // area scan done?
						finish_area_scan ();
						goto finish_now;
//						break;
					}
					scan.iiy = scan.dny;
					scan.cfs_dy = scan.fs_dy;
					if (AS_ch2nd_constheight_enabled) {
						scan.sstate = AS_SCAN_YP;
						AS_ch2nd_constheight_enabled = 0;  // disable now
					}else
						scan.sstate = AS_ch2nd_scan_switch; // switch to next YP or 2nd stage scan line
				}
				scan.cfs_dx = scan.fs_dx;
			}
		} while (--scan.ifr > 0 && scan.sstate == AS_SCAN_XM);
	finish_now:
		break;

	case AS_SCAN_2ND_XP: // configure 2nd XP scan
		AS_ch2nd_constheight_enabled = 1;  // enable now
		// disable feedback!!
		// config normal XP again
		scan.ix  = scan.nx;
		scan.iix = scan.dnx + PIPE_LEN;
		scan.cfs_dx = scan.fs_dx;
		scan.sstate = AS_SCAN_XP;
		clear_summing_data ();
		break;
	case AS_SCAN_2ND_XM: // configure 2nd XP scan
		AS_ch2nd_constheight_enabled = 1;  // enable now
		// config normal XM again
		scan.ix  = scan.nx;
		scan.iix = scan.dnx + PIPE_LEN;
		scan.cfs_dx = scan.fs_dx;
		scan.sstate = AS_SCAN_XM;
		clear_summing_data ();
		break;
	case AS_SCAN_YP: // "Y+" next line (could be Y-up or Y-dn, dep. on sign. of fs_dy!)
		if (scan.iiy--){
//						scan.xyz_vec[i_Y] -= scan.fs_dy;
			scan.xyz_vec[i_Y] = _lssub (scan.xyz_vec[i_Y], scan.cfs_dy);
		}
		else{
			if (scan.iy--){
				if (AS_jp >= 0){
					if (scan.raster_a){
						AS_ip = AS_kp;
						if ((++AS_jp) & 1){ // set start if X grid counter
							if (++AS_kp >= AS_mod)
								AS_kp=0;
							AS_ip += scan.raster_a;
						}
						if (AS_ip++ >= AS_mod)
							AS_ip -= AS_mod;
					} else
						if (++AS_jp == scan.dnx_probe){
							AS_jp = 0;
							AS_ip = scan.dnx_probe; // reset X grid counter
						}
				}
				scan.ix  = scan.nx;
				scan.iix = scan.dnx + PIPE_LEN;
				scan.sstate = AS_SCAN_XP;
				clear_summing_data ();
			}
			else // area scan is done -- double check here.
				finish_area_scan ();
			scan.cfs_dx = scan.fs_dx;
		}
		break;

	case AS_SCAN_YM: // "Y-" -- not valid yet -- exit AS
		finish_area_scan ();
		break;

	case AS_MOVE_XY: // move-XY: init/finalize scan, move to start/end of scan position, 
		// can be used with nx==0, then the job is finished after the xy move!!
		if (scan.num_steps_move_xy){
//			scan.xyz_vec[i_X] += scan.fm_dx;
//			scan.xyz_vec[i_Y] += scan.fm_dy;
			scan.xyz_vec[i_X] = _lsadd (scan.xyz_vec[i_X], scan.fm_dx);
			scan.xyz_vec[i_Y] = _lsadd (scan.xyz_vec[i_Y], scan.fm_dy);
			scan.num_steps_move_xy--;
		} else {
			if (scan.nx == 0){ // was MOVE_XY only?, then done
				finish_area_scan ();
			} else {
				scan.sstate = AS_SCAN_XP;
				scan.iix    = scan.dnx + PIPE_LEN;
				scan.ix     = scan.nx;
				scan.iy     = scan.ny;
				scan.fm_dx  = scan.fs_dx;
				scan.fm_dy  = -scan.fs_dy;
				scan.cfs_dx = scan.fs_dx;
				scan.cfs_dy = scan.fs_dy;
				clear_summing_data ();
			}
		}
		break;

	default: // just cancel job in case sth. went wrong!
		finish_area_scan ();
		break;
	}
}
