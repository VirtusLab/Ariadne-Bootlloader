/* Name: tftp.c
 * Author: .
 * Copyright: Arduino
 * License: GPL http://www.gnu.org/licenses/gpl-2.0.html
 * Project: ethboot
 * Function: tftp implementation and flasher
 * Version: 0.2 tftp / flashing functional
 */

#include <avr/pgmspace.h>
#include <util/delay.h>
#include <avr/boot.h>

#include "util.h"
#include "neteeprom.h"
#include "net.h"
#include "w5100_reg.h"
#include "tftp.h"
#include "validate.h"
#include "watchdog.h"
#include "serial.h"
#include "debug.h"
#include "debug_tftp.h"

/** Opcode?: tftp operation is unsupported. The bootloader only supports 'put' */
#define TFTP_OPCODE_ERROR_LEN 12
const unsigned char tftp_opcode_error_packet[] PROGMEM = "\12" "\0\5" "\0\0" "Opcode?";

/** Full: Binary image file is larger than the available space. */
#define TFTP_FULL_ERROR_LEN 9
const unsigned char tftp_full_error_packet[] PROGMEM = "\x09" "\0\5" "\0\3" "Full";

/** General catch-all error for unknown errors */
#define TFTP_UNKNOWN_ERROR_LEN 10
const unsigned char tftp_unknown_error_packet[] PROGMEM = "\10" "\0\5" "\0\0" "Error";

/** Invalid image file: Doesn't look like a binary image file */
#define TFTP_INVALID_IMAGE 23
const unsigned char tftp_invalid_image_packet[] PROGMEM = "\23" "\0\5" "\0\0" "Invalid image file";

uint16_t lastPacket = 0, highPacket = 0;


static void sockInit(uint16_t port)
{
	netWriteReg(REG_S3_CR, CR_CLOSE);

	do {
		// Write TFTP Port
		netWriteWord(REG_S3_PORT0, port);
		// Write mode
		netWriteReg(REG_S3_MR, MR_UDP);
		// Open Socket
		netWriteReg(REG_S3_CR, CR_OPEN);

		// Read Status
		if(netReadReg(REG_S3_SR) != SOCK_UDP)
			// Close Socket if it wasn't initialized correctly
			netWriteReg(REG_S3_CR, CR_CLOSE);

		// If socket correctly opened continue
	} while(netReadReg(REG_S3_SR) != SOCK_UDP);
}


#ifdef DEBUG_TFTP
static uint8_t processPacket(uint16_t packetSize)
#else
static uint8_t processPacket(void)
#endif
{

	uint8_t buffer[TFTP_PACKET_MAX_SIZE];
	uint16_t readPointer;
	address_t writeAddr;
	// Transfer entire packet to RAM
	uint8_t* bufPtr = buffer;
	uint16_t count;

	DBG_TFTP(
		tracePGMlnTftp(mTftpDebug_START);
		tracenum(packetSize);

		if(packetSize >= 0x800) tracePGMlnTftp(mTftpDebug_OVFL);

		DBG_BTN(button();)
	)

	// Read data from chip to buffer
	readPointer = netReadWord(REG_S3_RX_RD0);
	
	DBG_TFTP_EX(
		tracePGMlnTftp(mTftpDebug_RPTR);
		tracenum(readPointer);
	)

	if(readPointer == 0) readPointer += S3_RX_START;

	for(count = TFTP_PACKET_MAX_SIZE; count--;) {

		DBG_TFTP_EX(
			if((count == TFTP_PACKET_MAX_SIZE - 1) || (count == 0)) {
				tracePGMlnTftp(mTftpDebug_RPOS);
				tracenum(readPointer);
			}
		)

		*bufPtr++ = netReadReg(readPointer++);

		if(readPointer == S3_RX_END) readPointer = S3_RX_START;
	}

	netWriteWord(REG_S3_RX_RD0, readPointer);     // Write back new pointer
	netWriteReg(REG_S3_CR, CR_RECV);

	while(netReadReg(REG_S3_CR));

	DBG_TFTP_EX(
		tracePGMlnTftp(mTftpDebug_BLEFT);
		tracenum(netReadWord(REG_S3_RX_RSR0));
	)

	DBG_TFTP_EX(
		// Dump packet
		bufPtr = buffer;
		tracePGM(mTftpDebug_NEWLINE);

		for(count = TFTP_PACKET_MAX_SIZE / 2; count--;) {
			uint16_t val = *bufPtr++;
			val |= (*bufPtr++) << 8;
			tracenum(val);

			if((count % 8) == 0 && count != 0) tracePGM(mTftpDebug_NEWLINE);
			else tracePGM(mTftpDebug_SPACE);
		}
	)

	// Set up return IP address and port
	uint8_t i;

	for(i = 0; i < 6; i++) netWriteReg(REG_S3_DIPR0 + i, buffer[i]);

	DBG_TFTP(tracePGMlnTftp(mTftpDebug_RADDR);)
	
	// Parse packet
	uint16_t tftpDataLen = (buffer[6] << 8) + buffer[7];
	uint16_t tftpOpcode = (buffer[8] << 8) + buffer[9];
	uint16_t tftpBlock = (buffer[10] << 8) + buffer[11];
	
	DBG_TFTP(
		tracePGMlnTftp(mTftpDebug_BLOCK);
		tracenum(tftpBlock);
		tracePGM(mTftpDebug_OPCODE);
		tracenum(tftpOpcode);
		tracePGM(mTftpDebug_DLEN);
		tracenum(tftpDataLen - (TFTP_OPCODE_SIZE + TFTP_BLOCKNO_SIZE));
	)

	if((tftpOpcode == TFTP_OPCODE_DATA)
		&& ((tftpBlock > MAX_ADDR / 0x200) || (tftpBlock < highPacket) || (tftpBlock > highPacket + 1)))
		tftpOpcode = TFTP_OPCODE_UKN;

	if(tftpDataLen > (0x200 + TFTP_OPCODE_SIZE + TFTP_BLOCKNO_SIZE))
		tftpOpcode = TFTP_OPCODE_UKN;

	uint8_t returnCode = ERROR_UNKNOWN;
	uint16_t packetLength;


	switch(tftpOpcode) {

		case TFTP_OPCODE_RRQ: // Read request
			DBG_TFTP(tracePGMlnTftp(mTftpDebug_OPRRQ);)
			break;

		case TFTP_OPCODE_WRQ: // Write request
			// Valid WRQ -> reset timer
			resetTick();

			DBG_TFTP(tracePGMlnTftp(mTftpDebug_OPWRQ);)

			// Flagging image as invalid since the flashing process has started
			eeprom_write_byte(EEPROM_IMG_STAT, EEPROM_IMG_BAD_VALUE);

#ifdef TFTP_RANDOM_PORT
			sockInit((buffer[4] << 8) | ~buffer[5]); // Generate a 'random' TID (RFC1350)
#else
			sockInit(tftpTransferPort);
#endif

			DBG_TFTP(
				tracePGMlnTftp(mTftpDebug_NPORT);
#ifdef TFTP_RANDOM_PORT
				tracenum((buffer[4] << 8) | (buffer[5] ^ 0x55));
#else
				tracenum(tftpTransferPort);
#endif
			)
			
			lastPacket = highPacket = 0;
			returnCode = ACK; // Send back acknowledge for packet 0
			break;

		case TFTP_OPCODE_DATA:
			// Valid Data Packet -> reset timer
			resetTick();
			
			DBG_TFTP(tracePGMlnTftp(mTftpDebug_OPDATA);)
			
			packetLength = tftpDataLen - (TFTP_OPCODE_SIZE + TFTP_BLOCKNO_SIZE);
			lastPacket = tftpBlock;
			writeAddr = (tftpBlock - 1) << 9; // Flash write address for this block

			if((writeAddr + packetLength) > MAX_ADDR) {
				// Flash is full - abort with an error before a bootloader overwrite occurs
				// Application is now corrupt, so do not hand over.

				DBG_TFTP(tracePGMlnTftp(mTftpDebug_FULL);)

				returnCode = ERROR_FULL;
			} else {
				
				DBG_TFTP(
					tracePGMlnTftp(mTftpDebug_WRADDR);
					tracenum(writeAddr);
				)
				
				uint8_t* pageBase = buffer + (UDP_HEADER_SIZE + TFTP_OPCODE_SIZE + TFTP_BLOCKNO_SIZE); // Start of block data
				uint16_t offset = 0; // Block offset


				// Set the return code before packetLength gets rounded up
				if(packetLength < TFTP_DATA_SIZE) returnCode = FINAL_ACK;
				else returnCode = ACK;

				// Round up packet length to a full flash sector size
				while(packetLength % SPM_PAGESIZE) packetLength++;

				DBG_TFTP(
					tracePGMlnTftp(mTftpDebug_PLEN);
					tracenum(packetLength);
				)

				if(writeAddr == 0) {
					// First sector - validate
					if(!validImage(pageBase)) {
						returnCode = INVALID_IMAGE;
						/* FIXME: Validity checks. Small programms (under 512 bytes?) don't
						 * have the the JMP sections and that is why app.bin was failing.
						 * When flashing big binaries is fixed, uncomment the break below.*/
#ifndef DEBUG_TFTP
						break;
#endif
					}
				}

				// Flash packets
				for(offset = 0; offset < packetLength;) {
					uint16_t writeValue = (pageBase[offset]) | (pageBase[offset + 1] << 8);
					boot_page_fill(writeAddr + offset, writeValue);

					DBG_TFTP_EX(
						if((offset == 0) || ((offset == (packetLength - 2)))) {
							tracePGMlnTftp(mTftpDebug_WRITE);
							tracenum(writeValue);
							tracePGM(mTftpDebug_OFFSET);
							tracenum(writeAddr + offset);
						}
					)

					offset += 2;

					if(offset % SPM_PAGESIZE == 0) {
						boot_page_erase(writeAddr + offset - SPM_PAGESIZE);
						boot_spm_busy_wait();
						boot_page_write(writeAddr + offset - SPM_PAGESIZE);
						boot_spm_busy_wait();
#if defined(RWWSRE)
						// Reenable read access to flash
						boot_rww_enable();
#endif
					}
				}

				if(returnCode == FINAL_ACK) {
					// Flash is complete
					// Hand over to application

					DBG_TFTP(tracePGMlnTftp(mTftpDebug_DONE);)

					// Flag the image as valid since we received the last packet
					eeprom_write_byte(EEPROM_IMG_STAT, EEPROM_IMG_OK_VALUE);
				}
			}

			break;

		// Acknowledgment
		case TFTP_OPCODE_ACK:

			DBG_TFTP(tracePGMlnTftp(mTftpDebug_OPACK);)

			break;

		// Error signal
		case TFTP_OPCODE_ERROR:

			DBG_TFTP(tracePGMlnTftp(mTftpDebug_OPERR);)

			/* FIXME: Resetting might be needed here too */
			break;

		default:
			DBG_TFTP(
				tracePGMlnTftp(mTftpDebug_INVOP);
				tracenum(tftpOpcode);
			)

#ifdef TFTP_RANDOM_PORT
			sockInit((buffer[4] << 8) | ~buffer[5]); // Generate a 'random' TID (RFC1350)
#else
			sockInit(tftpTransferPort);
#endif
			/* FIXME: This is where the tftp server should be resetted.
			 * It can be done by reinitializig the tftpd or
			 * by resetting the device. I should find out which is best...
			 * Right now it is being done by resetting the timer if we have a
			 * data packet. */
			// Invalid - return error
			returnCode = ERROR_INVALID;
			break;

	}

	return(returnCode);
}


static void sendResponse(uint16_t response)
{
	uint8_t txBuffer[100];
	uint8_t* txPtr = txBuffer;
	uint8_t packetLength;
	uint16_t writePointer;

	writePointer = netReadWord(REG_S3_TX_WR0) + S3_TX_START;

	switch(response) {
		default:

		case ERROR_UNKNOWN:
			// Send unknown error packet
			packetLength = TFTP_UNKNOWN_ERROR_LEN;
			memcpy_P(txBuffer, tftp_unknown_error_packet, packetLength);
			break;

		case ERROR_INVALID:
			// Send invalid opcode packet
			packetLength = TFTP_OPCODE_ERROR_LEN;
			memcpy_P(txBuffer, tftp_opcode_error_packet, packetLength);
			break;

		case ERROR_FULL:
			// Send unknown error packet
			packetLength = TFTP_FULL_ERROR_LEN;
			memcpy_P(txBuffer, tftp_full_error_packet, packetLength);
			break;

		case ACK:
			if(lastPacket > highPacket) highPacket = lastPacket;

			DBG_TFTP(tracePGMlnTftp(mTftpDebug_SACK);)
			/* no break */

		case FINAL_ACK:
			DBG_TFTP(
				if(response == FINAL_ACK)
					tracePGMlnTftp(mTftpDebug_SFACK);
			)

			packetLength = 4;
			*txPtr++ = TFTP_OPCODE_ACK >> 8;
			*txPtr++ = TFTP_OPCODE_ACK & 0xff;
			// lastPacket is block code
			*txPtr++ = lastPacket >> 8;
			*txPtr = lastPacket & 0xff;
			break;
	}

	txPtr = txBuffer;

	while(packetLength--) {
		netWriteReg(writePointer++, *txPtr++);

		if(writePointer == S3_TX_END) writePointer = S3_TX_START;
	}

	netWriteWord(REG_S3_TX_WR0, writePointer - S3_TX_START);
	netWriteReg(REG_S3_CR, CR_SEND);

	while(netReadReg(REG_S3_CR));

	DBG_TFTP(tracePGMlnTftp(mTftpDebug_RESP);)
}


/**
 * Initializes the network controller
 */
void tftpInit(void)
{
	// Open socket
	sockInit(TFTP_PORT);

#ifndef TFTP_RANDOM_PORT
	if(eeprom_read_byte(EEPROM_SIG_3) == EEPROM_SIG_3_VALUE)
		tftpTransferPort = ((eeprom_read_byte(EEPROM_PORT + 1) << 8) + eeprom_read_byte(EEPROM_PORT));
	else
		tftpTransferPort = TFTP_STATIC_PORT;
#endif
	
	DBG_TFTP(
		tracePGMlnTftp(mTftpDebug_INIT);
#ifndef TFTP_RANDOM_PORT
		tracePGMlnTftp(mTftpDebug_PORT);
		tracenum(tftpTransferPort);
#endif
	)
}


/**
 * Looks for a connection
 */
uint8_t tftpPoll(void)
{
	uint8_t response = ACK;
	// Get the size of the recieved data
	uint16_t packetSize = netReadWord(REG_S3_RX_RSR0);
//	uint16_t packetSize = 0, incSize = 0;

// 	do {
// 		incSize = netReadWord(REG_S3_RX_RSR0);
// 		if(incSize != 0) {
// 			_delay_ms(400);
// 			packetSize = netReadWord(REG_S3_RX_RSR0);
// 		}
// 	} while (packetSize != incSize);

	if(packetSize) {
		tftpFlashing = TRUE;

		while((netReadReg(REG_S3_IR) & IR_RECV)) {
			netWriteReg(REG_S3_IR, IR_RECV);
			//FIXME: is this right after all? smaller delay but
			//still a delay and it still breaks occasionally
			_delay_ms(TFTP_PACKET_DELAY);
		}

		// Process Packet and get TFTP response code
#ifdef DEBUG_TFTP
		packetSize = netReadWord(REG_S3_RX_RSR0);
		response = processPacket(packetSize);
#else
		response = processPacket();
#endif
		// Send the response
		sendResponse(response);
	}

	if(response == FINAL_ACK) {
		netWriteReg(REG_S3_CR, CR_CLOSE);
		// Complete
		return(0);
	}

	// Tftp continues
	return(1);
}

