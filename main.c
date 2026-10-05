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

#define PACKET_LEN 96						// packet length in bits
#define LARGE_BUF_LEN (PACKET_LEN * 10 + 5) // 96 bits, 10 samples per bit (symbol), 5 extra samples for the correlator max search
#define TIME_PACKET 0x60
#define SKIP_CONFIRMED (1500 - 50) // syncword sliced correctly - skip to just before the next 3s slot (margin covers clock error and jitter)
#define SKIP_UNCONFIRMED 20		   // possible false trigger - only step past the current correlation peak

const int8_t sync[16] = {-1, 1, -1, 1, -1, 1, -1, 1, -1, 1, -1, 1, -1, 1, -1, 1}; // sync symbol transitions

int16_t s[LARGE_BUF_LEN]; // a whole 1.92s frame should fit in (50bps, 10 samples per symbol)
uint16_t s_idx;			  // circular buffer index
int16_t *symbols[16];
uint8_t skip_samples;
uint16_t skip_cnt;
uint16_t skip_len;

uint8_t raw_packet[PACKET_LEN / 8];

typedef enum
{
	PCSK_DEC_CRC_OK,
	PCSK_DEC_RS_OK,
	PCSK_DEC_FAILED,
	PCSK_UNK_PACKET
} decode_status_t;

typedef struct
{
	time_t timestamp;
	uint8_t tz;
} pcsk_packet_t;

uint8_t show_all_types = 0; // show all frame types (1) or time sync only (0)
// uint8_t dump_rs = 0;										  // dump Reed-Solomon symbols?
// uint8_t crc_flt = 0;										  // filter out time messages with CRC mismatch?
const uint8_t scram[5] __attribute__((nonstring)) = "\nGUM+"; // scrambler sequence
const uint32_t epoch = 946684800U;							  // 01-01-2000 00:00:00
const uint8_t rs_poly[5] = {1, 1, 0, 0, 1};					  // RS(15, 9) polynomial, dec=19
pcsk_packet_t dec_packet;									  // decoded packet

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

static uint32_t extract_raw_time(const uint8_t *p, uint8_t *tz)
{
	*tz = ((p[7] >> 4) & 2) | ((p[7] >> 6) & 1);
	return ((uint32_t)(((p[3] << 1) & 0x3F) | (p[4] >> 7)) << 24) |
		   ((uint32_t)(((p[4] << 1) | (p[5] >> 7)) & 0xFF) << 16) |
		   ((uint32_t)(((p[5] << 1) | (p[6] >> 7)) & 0xFF) << 8) |
		   (uint32_t)(((p[6] << 1) | (p[7] >> 7)) & 0xFF);
}

decode_status_t pcsk_decode(pcsk_packet_t *pkt, uint8_t raw[12])
{
	if (raw[2] != TIME_PACKET)
		return PCSK_UNK_PACKET;

	uint8_t *data = &raw[3];
	decode_status_t status = PCSK_DEC_CRC_OK;

	// CRC and RS both operate on the *scrambled* (on-air) bytes
	if (CRC8(0x07, 0x00, data, 5) != raw[11])
	{
		uint8_t cw[15] = {
			(raw[3] >> 1) & 0xF, ((raw[4] >> 5) & 0x7) | ((raw[3] & 1) << 3),
			(raw[4] >> 1) & 0xF, ((raw[5] >> 5) & 0x7) | ((raw[4] & 1) << 3),
			(raw[5] >> 1) & 0xF, ((raw[6] >> 5) & 0x7) | ((raw[5] & 1) << 3),
			(raw[6] >> 1) & 0xF, ((raw[7] >> 5) & 0x7) | ((raw[6] & 1) << 3),
			(raw[7] >> 1) & 0xF,
			raw[8] >> 4, raw[8] & 0xF, raw[9] >> 4, raw[9] & 0xF, raw[10] >> 4, raw[10] & 0xF};

		rs_status_t r = decode_RS(&rs, cw);
		if (r != RS_NO_ERROR && r != RS_CORRECTED)
			return PCSK_DEC_FAILED;

		uint8_t upper = raw[3] & 0xE0, lower = raw[7] & 0x01;
		raw[3] = upper | (cw[0] << 1) | (cw[1] >> 3);
		raw[4] = ((cw[1] & 7) << 5) | (cw[2] << 1) | (cw[3] >> 3);
		raw[5] = ((cw[3] & 7) << 5) | (cw[4] << 1) | (cw[5] >> 3);
		raw[6] = ((cw[5] & 7) << 5) | (cw[6] << 1) | (cw[7] >> 3);
		raw[7] = ((cw[7] & 7) << 5) | (cw[8] << 1) | lower;

		if (CRC8(0x07, 0x00, data, 5) != raw[11])
			return PCSK_DEC_FAILED;
		status = PCSK_DEC_RS_OK;
	}

	for (uint8_t i = 0; i < 5; i++)
		data[i] ^= scram[i];

	uint8_t tz;
	uint32_t raw_t = extract_raw_time(raw, &tz) * 3;
	pkt->timestamp = (time_t)epoch + raw_t + 3600 * tz;
	pkt->tz = tz;
	return status;
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

			/*else if (strcmp(argv[i], "-rs") == 0)
			{
				printf("\033[95mINFO:\033[39m Showing Reed-Solomon code symbols\n");
				dump_rs = 1;
			}*/

			/*else if (strcmp(argv[i], "-crc") == 0)
			{
				printf("\033[95mINFO:\033[39m Showing time packets with correct CRC only\n");
				crc_flt = 1;
			}*/
		}
	}

	rs_init_t rv = init_RS(&rs, 15, 9, (uint8_t *)rs_poly);
	if (rv != RS_INIT_OK)
	{
		printf("\033[95mERROR:\033[39m Can not initialize Reed-Solomon decoder (err:%u).\n", rv);
		return 1;
	}

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

				// hardcoded symbol excursion threshold. this is half of the symbol deviation (+/-1.0 at +/-16,384)
				const int32_t thresh = 8192;
				// detect the syncword, then check if the first symbol is a negative spike
				if (corr > 16 * thresh && *symbols[0] < -thresh)
				{
					// look at a few samples ahead to find maximum correlation value
					int32_t corr_max = corr;
					uint8_t shift_max = 0;

					for (uint8_t shift = 1; shift <= 5; shift++)
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
					memset(raw_packet, 0, PACKET_LEN / 8);

					for (uint16_t i = 0; i < PACKET_LEN; i++)
					{
						int16_t symb = s[(s_idx + shift_max + i * 10) % LARGE_BUF_LEN];
						if (abs(symb) > thresh) // hardcoded threshold
							b = !b;

						raw_packet[i / 8] |= (b << (7 - (i % 8)));
					}

					// attempt decode
					decode_status_t pcsk_res = pcsk_decode(&dec_packet, raw_packet);

					// get local time
					now = time(NULL);
					tm_now = localtime(&now);

					if (pcsk_res == PCSK_DEC_CRC_OK || pcsk_res == PCSK_DEC_RS_OK)
					{
						// print type
						printf("\033[96m[%02d:%02d:%02d] \033[92mPacket received\033[39m\n", tm_now->tm_hour, tm_now->tm_min, tm_now->tm_sec);
						printf(" ├ \033[93mType:\033[39m ");
						printf("time\n");

						// print contents
						printf(" ├ \033[93mRaw data:\033[39m ");
						for (uint8_t i = 0; i < 12; i++)
							printf("%02X ", raw_packet[i]);
						printf("\n");

						// print the raw timestamp
						printf(" ├ \033[93mTimestamp:\033[39m %lld\n", (long long)(dec_packet.timestamp - epoch));

						// print decoded time
						struct tm *gt = gmtime(&dec_packet.timestamp);
						printf(" ├ \033[93mDecoded:\033[39m %04d-%02d-%02d %02d:%02d:%02d (UTC+%d)\n",
							   gt->tm_year + 1900,
							   gt->tm_mon + 1,
							   gt->tm_mday,
							   gt->tm_hour,
							   gt->tm_min,
							   gt->tm_sec,
							   dec_packet.tz);

						// print CRC check
						if (pcsk_res == PCSK_DEC_CRC_OK)
						{
							printf(" └ \033[93mCRC:\033[39m \033[92mmatch\033[39m\n");
						}

						// print post-RS CRC check
						if (pcsk_res == PCSK_DEC_RS_OK)
						{
							printf(" └ \033[93mPost-RS CRC:\033[39m \033[92mmatch\033[39m\n");
						}
					}
					else // uncorrectable data or an unknown packet type
					{
						if (show_all_types)
						{
							printf("\033[96m[%02d:%02d:%02d] \033[92mPacket received\033[39m\n", tm_now->tm_hour, tm_now->tm_min, tm_now->tm_sec);
							printf(" ├ \033[93mType:\033[39m ");
							printf("uncorrectable / other\n");

							// print raw contents
							printf(" └ \033[93mRaw data:\033[39m ");
							for (uint8_t i = 0; i < 12; i++)
								printf("%02X ", raw_packet[i]);
							printf("\n");
						}
					}

					// skip a whole slot only if the frame is confirmed by its syncword,
					// so that a false trigger does not blind the decoder to the next real frame
					uint8_t sync_ok = (raw_packet[0] == 0x55 && raw_packet[1] == 0x55);
					skip_len = sync_ok ? SKIP_CONFIRMED : SKIP_UNCONFIRMED;
					skip_samples = 1;
					skip_cnt = 0;
				}
			}
			else
			{
				skip_cnt++;
				// frames start every 3s (1500 samples) and the correlator looks at the oldest
				// samples in the buffer, so the skip is counted from the current frame's start
				if (skip_cnt >= skip_len)
				{
					skip_cnt = 0;
					skip_samples = 0;
				}
			}
		}
	}

	return 0;
}
