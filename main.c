#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <locale.h>

#include "rs-codes/rs.h"

#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

#define LARGE_BUF_LEN (12 * 8 * 10 + 5) // 12 bytes, 10 samples per bit (symbol), 5 extra samples for the correlator max search
#define TIME_PACKET 0x60

const int8_t sync[16] = {-1, 1, -1, 1, -1, 1, -1, 1, -1, 1, -1, 1, -1, 1, -1, 1}; // sync symbol transitions

int16_t s[LARGE_BUF_LEN]; // a whole 1.92s frame should fit in (50bps, 10 samples per symbol)
uint16_t s_idx;			  // circular buffer index
int16_t *symbols[16];
uint8_t skip_samples;
uint16_t skip_cnt;

uint8_t raw_packet[12];

uint8_t show_all_types = 0;									  // show all frame types (1) or time sync only (0)
uint8_t dump_rs = 0;										  // dump Reed-Solomon symbols?
uint8_t crc_flt = 0;										  // filter out time messages with CRC mismatch?
const uint8_t scram[5] __attribute__((nonstring)) = "\nGUM+"; // scrambler sequence
const uint32_t epoch = 946684800;							  // 01-01-2000 00:00:00
const uint8_t rs_poly[5] = {1, 1, 0, 0, 1};					  // RS(15, 9) polynomial, dec=19

rs_t rs; // RS(15, 9) struct

time_t now;
struct tm *tm_now;

uint8_t CRC8(uint8_t poly, uint8_t init, const uint8_t *in, uint16_t len)
{
	uint16_t crc = init; // init val

	for (uint16_t i = 0; i < len; i++)
	{
		crc ^= in[i];
		for (uint8_t j = 0; j < 8; j++)
		{
			crc <<= 1;
			if (crc & 0x100)
				crc = (crc ^ poly) & 0xFF;
		}
	}

	return crc & 0xFF;
}

int main(int argc, char *argv[])
{
#ifdef _WIN32
	_setmode(_fileno(stdin), _O_BINARY);
	SetConsoleOutputCP(CP_UTF8);
	setlocale(LC_ALL, ".UTF-8");
#else
	setlocale(LC_ALL, "");
#endif

	if (argc > 1)
	{
		for (uint8_t i = 1; i < argc; i++)
		{
			if (strcmp(argv[i], "-a") == 0)
			{
				printf("\033[95mINFO:\033[39m Decoding all packet types\n");
				show_all_types = 1;
			}

			else if (strcmp(argv[i], "-rs") == 0)
			{
				printf("\033[95mINFO:\033[39m Showing Reed-Solomon code symbols\n");
				dump_rs = 1;
			}

			else if (strcmp(argv[i], "-crc") == 0)
			{
				printf("\033[95mINFO:\033[39m Showing time packets with correct CRC only\n");
				crc_flt = 1;
			}
		}
	}

	init_RS(&rs, 15, 9, (uint8_t *)rs_poly);

	for (uint8_t i = 0; i < 16; i++)
		symbols[i] = &s[i * 10];

	while (1)
	{
		if (fread(&s[s_idx], sizeof(*s), 1, stdin) != 1)
		{
			if (feof(stdin))
			{
				// get local time
				now = time(NULL);
				tm_now = localtime(&now);
				printf("\033[96m[%02d:%02d:%02d] \033[92mEOF reached\033[39m\n", tm_now->tm_hour, tm_now->tm_min, tm_now->tm_sec);
			}
			else
			{
				perror("fread");
			}
			break;
		}
		else
		{
			// advance the circular buffer index
			s_idx++;
			if (s_idx >= LARGE_BUF_LEN)
				s_idx = 0;

			// advance pointers to symbols
			for (uint8_t i = 0; i < 16; i++)
			{
				if (symbols[i] < &s[LARGE_BUF_LEN - 1])
					symbols[i]++;
				else
					symbols[i] = s;
			}

			if (!skip_samples)
			{
				// correlate against syncword
				int32_t corr = 0;
				for (uint16_t i = 0; i < 16; i++)
					corr += *symbols[i] * sync[i];

				// hardcoded symbol excursion threshold. TODO: base these values on std dev
				const int32_t thresh = 5000;
				// detect the syncword, then check if the first symbol is a negative spike
				if (corr > 16 * thresh && *symbols[0] < -thresh)
				{
					// look at a few samples ahead to find maximum correlation value
					int32_t corr_max = corr;
					uint8_t shift = 1;
					uint8_t shift_max = 0;

					for (; shift <= 5; shift++)
					{
						int32_t corr_s = 0;

						for (uint16_t i = 0; i < 16; i++)
						{
							uint16_t idx = (symbols[i] - s + shift) % LARGE_BUF_LEN;
							corr_s += s[idx] * sync[i];
						}

						if (corr_s > corr_max)
						{
							corr_max = corr_s;
							shift_max = shift;
						}
						else
						{
							break;
						}
					}

					// demodulate the signal
					uint8_t b = 1;
					memset(raw_packet, 0, sizeof(raw_packet));

					for (uint16_t i = 0; i < 96; i++)
					{
						int16_t symb = s[(s_idx + shift_max + i * 10) % LARGE_BUF_LEN];
						if (abs(symb) > thresh) // hardcoded threshold
							b = !b;

						raw_packet[i / 8] |= (b << (7 - (i % 8)));
					}

					// retrieve data
					uint8_t packet_type = raw_packet[2];
					uint8_t *data_start = &raw_packet[3];
					uint8_t rcv_crc = raw_packet[11];

					// calculate CRC (it is unprotected by the RS code...)
					uint8_t calc_crc = CRC8(0x07, 0x00, data_start, 5);

					// if CRC filtering is enabled and we just received a time packet - ignore it
					if (crc_flt == 1 && packet_type == TIME_PACKET && rcv_crc != calc_crc)
					{
						skip_samples = 1;
						skip_cnt = 0;
						continue;
					}

					if (show_all_types || packet_type == TIME_PACKET)
					{
						// get local time
						now = time(NULL);
						tm_now = localtime(&now);

						// packet has been detected
						printf("\033[96m[%02d:%02d:%02d] \033[92mPacket received\033[39m\n", tm_now->tm_hour, tm_now->tm_min, tm_now->tm_sec);

						// print type
						printf(" ├ \033[93mType:\033[39m ");
						if (packet_type == TIME_PACKET)
							printf("time\n");
						else
							printf("other (0x%02X)\n", packet_type);

						if (packet_type == TIME_PACKET)
						{
							// print raw contents
							printf(" ├ \033[93mRaw data:\033[39m ");
							for (uint8_t i = 0; i < 12; i++)
								printf("%02X ", raw_packet[i]);
							printf("\n");

							// extract RS(15, 9) codeword
							uint8_t cword[15] =
								{
									(raw_packet[3] >> 1) & 0xF,
									((raw_packet[4] >> 5) & 0x7) | ((raw_packet[3] & 1) << 3),
									(raw_packet[4] >> 1) & 0xF,
									((raw_packet[5] >> 5) & 0x7) | ((raw_packet[4] & 1) << 3),
									(raw_packet[5] >> 1) & 0xF,
									((raw_packet[6] >> 5) & 0x7) | ((raw_packet[5] & 1) << 3),
									(raw_packet[6] >> 1) & 0xF,
									((raw_packet[7] >> 5) & 0x7) | ((raw_packet[6] & 1) << 3),
									(raw_packet[7] >> 1) & 0xF,
									(raw_packet[8] >> 4) & 0xF, raw_packet[8] & 0xF,
									(raw_packet[9] >> 4) & 0xF, raw_packet[9] & 0xF,
									(raw_packet[10] >> 4) & 0xF, raw_packet[10] & 0xF};

							// dump RS symbols
							if (dump_rs)
							{
								printf(" ├ \033[93mReceived RS symbols:\033[39m  %02d %02d %02d %02d %02d %02d %02d %02d %02d | %02d %02d %02d %02d %02d %02d\n",
									   cword[0], cword[1], cword[2], cword[3], cword[4],
									   cword[5], cword[6], cword[7], cword[8], cword[9],
									   cword[10], cword[11], cword[12], cword[13], cword[14]);
							}

							// apply error correction (it overwrites the buffer)
							decode_RS(&rs, (int8_t *)cword);

							// dump RS symbols again
							if (dump_rs)
							{
								printf(" ├ \033[93mCorrected RS symbols:\033[39m %02d %02d %02d %02d %02d %02d %02d %02d %02d | %02d %02d %02d %02d %02d %02d\n",
									   cword[0], cword[1], cword[2], cword[3], cword[4],
									   cword[5], cword[6], cword[7], cword[8], cword[9],
									   cword[10], cword[11], cword[12], cword[13], cword[14]);
							}

							// put the corrected data back (raw_packet[] is not raw anymore :)
							// TODO: there is something wrong with the RS decoder and applying this causes some timestamps to be off
							/*uint8_t upper = raw_packet[3] & 0xE0; // 3 unprotected bits (start)
							uint8_t lower = raw_packet[7] & 0x01; // 1 unprotected bit (end)

							raw_packet[3] = upper | (cword[0] << 1) | (cword[1] >> 3);
							raw_packet[4] = ((cword[1] & 0x07) << 5) | (cword[2] << 1) | (cword[3] >> 3);
							raw_packet[5] = ((cword[3] & 0x07) << 5) | (cword[4] << 1) | (cword[5] >> 3);
							raw_packet[6] = ((cword[5] & 0x07) << 5) | (cword[6] << 1) | (cword[7] >> 3);
							raw_packet[7] = ((cword[7] & 0x07) << 5) | (cword[8] << 1) | lower;*/

							// descramble contents
							for (uint8_t i = 0; i < 5; i++)
								raw_packet[3 + i] ^= scram[i];

							// extract the 30-bit timestamp
							uint32_t raw_t =
								((uint32_t)(((raw_packet[3] << 1) & 0x3F) | (raw_packet[4] >> 7)) << 24) |
								((uint32_t)(((raw_packet[4] << 1) | (raw_packet[5] >> 7)) & 0xFF) << 16) |
								((uint32_t)(((raw_packet[5] << 1) | (raw_packet[6] >> 7)) & 0xFF) << 8) |
								(uint32_t)(((raw_packet[6] << 1) | (raw_packet[7] >> 7)) & 0xFF);

							// convert the timestamp into seconds since 01-01-2000 (each tick is 3s)
							uint8_t tz = ((raw_packet[7] >> 4) & 2) | ((raw_packet[7] >> 6) & 1);
							raw_t *= 3;
							raw_t += 3600 * tz;

							// print the timestamp
							printf(" ├ \033[93mTimestamp:\033[39m %u\n", raw_t);
							time_t eczas = epoch + raw_t;

							// print decoded time
							struct tm *gt = gmtime(&eczas);
							printf(" ├ \033[93mDecoded:\033[39m %04d-%02d-%02d %02d:%02d:%02d (UTC+%d)\n",
								   gt->tm_year + 1900,
								   gt->tm_mon + 1,
								   gt->tm_mday,
								   gt->tm_hour,
								   gt->tm_min,
								   gt->tm_sec,
								   tz);

							// print CRC
							printf(" └ \033[93mCRC:\033[39m");
							if (rcv_crc == calc_crc)
								printf(" \033[92mmatch\033[39m\n");
							else
								printf(" \033[91mmismatch\033[39m\n");
						}
						else // other packets
						{
							// print raw contents
							printf(" └ \033[93mRaw data:\033[39m ");
							for (uint8_t i = 0; i < 12; i++)
								printf("%02X ", raw_packet[i]);
							printf("\n");
						}
					}

					skip_samples = 1;
					skip_cnt = 0;
				}
			}
			else
			{
				skip_cnt++;
				// 3.0-1.92=1.08; 1.08/0.02 is 54, so we skip 52 symbols to end up right before the next syncword
				if (skip_cnt == 52 * 10)
				{
					skip_cnt = 0;
					skip_samples = 0;
				}
			}
		}
	}

	return 0;
}
