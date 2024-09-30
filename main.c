#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "rs-codes/rs.h"

const int8_t sync[16]={-1, 1, -1, 1, -1, 1, -1, 1, -1, 1, -1, 1, -1, 1, -1, 1}; //sync symbol transitions
int16_t s[12*8*10+1]; //a whole 1.92s frame should fit in (50bps, 10 samples per symbol)
uint8_t skip_samples;
uint16_t skip_cnt;

uint8_t raw_packet[12];

uint8_t show_all=0;						//show all frames (1) or time sync only (0)
uint8_t dump_rs=0;						//dump Reed-Solomon symbols?
const uint8_t scram[5]="\nGUM+";		//scrambler sequence
const uint32_t epoch=946684800;			//01-01-2000 00:00:00
const uint8_t rs_poly[5]={1,1,0,0,1};	//RS(15, 9) polynomial, dec=19

rs_t rs;								//RS(15, 9) struct

uint8_t CRC8(const uint8_t poly, const uint8_t init, const uint8_t *in, const uint16_t len)
{
	uint16_t crc=init; //init val

	for(uint16_t i=0; i<len; i++)
	{
		crc^=in[i];
		for(uint8_t j=0; j<8; j++)
		{
			crc<<=1;
			if(crc&0x100)
				crc=(crc^poly)&0xFF;
		}
	}

	return crc&0xFF;
}

int main(int argc, char* argv[])
{
	if(argc>1)
	{
		for(uint8_t i=1; i<argc; i++)
		{
			if(strstr(argv[i], "-a"))
			{
				printf("\033[95mINFO:\033[39m Showing all packets\n");
				show_all=1;
			}

			else if(strstr(argv[i], "-rs"))
			{
				printf("\033[95mINFO:\033[39m Showing Reed-Solomon code symbols\n");
				dump_rs=1;
			}
		}
	}

	init_RS(&rs, 15, 9, (uint8_t*)rs_poly);

	while(1)
	{
		while(fread((uint8_t*)&s[sizeof(s)/sizeof(int16_t)-1], sizeof(int16_t), 1, stdin)<1);

		//shift left
		for(uint16_t i=0; i<sizeof(s)/sizeof(int16_t)-1; i++)
			s[i]=s[i+1];

		if(!skip_samples)
		{
			//correlate against syncword
			int32_t corr=0;
			for(uint16_t i=0; i<16*10; i+=10)
				corr+=s[i]*sync[i/10];

			if(corr>320000 && s[0]<-10000) //10e3 is hardcoded TODO: base this value on std dev
			{
				uint8_t b=1;
				memset(raw_packet, 0, sizeof(raw_packet));

				for(uint16_t i=0; i<96; i++)
				{
					if(abs(s[i*10])>10000)
						b=!b;

					raw_packet[i/8]|=(b<<(7-(i%8)));
				}

				if(show_all || raw_packet[2]==0x60)
				{
					//get local time
					time_t now = time(NULL);
					struct tm* tm_now = localtime(&now);

					//packet has been detected
					printf("\033[96m[%02d:%02d:%02d] \033[92mPacket received\033[39m\n", tm_now->tm_hour, tm_now->tm_min, tm_now->tm_sec);

					//print type
					printf(" ├ \033[93mType:\033[39m ");
					if(raw_packet[2]==0x60)
						printf("time\n");
					else
						printf("other\n");

					if(raw_packet[2]==0x60)
					{
						//print raw contents
						printf(" ├ \033[93mRaw data:\033[39m ");
						for(uint8_t i=0; i<12; i++)
							printf("%02X ", raw_packet[i]);
						printf("\n");

						//calculate CRC
						uint8_t calc_crc=CRC8(0x07, 0x00, &raw_packet[3], 5);

						//extract RS(15, 9) codeword
						uint8_t cword[15]=
						{
							(raw_packet[3]>>1)&0xF,
							((raw_packet[4]>>5)&0x7)|((raw_packet[3]&1)<<3),
							(raw_packet[4]>>1)&0xF,
							((raw_packet[5]>>5)&0x7)|((raw_packet[4]&1)<<3),
							(raw_packet[5]>>1)&0xF,
							((raw_packet[6]>>5)&0x7)|((raw_packet[5]&1)<<3),
							(raw_packet[6]>>1)&0xF,
							((raw_packet[7]>>5)&0x7)|((raw_packet[6]&1)<<3),
							(raw_packet[7]>>1)&0xF,
							(raw_packet[8]>>4)&0xF, raw_packet[8]&0xF,
							(raw_packet[9]>>4)&0xF, raw_packet[9]&0xF,
							(raw_packet[10]>>4)&0xF, raw_packet[10]&0xF
						};

						//dump RS symbols
						if(dump_rs)
						{
							printf(" ├ \033[93mReceived RS symbols:\033[39m  %02d %02d %02d %02d %02d %02d %02d %02d %02d | %02d %02d %02d %02d %02d %02d\n",
								cword[0], cword[1], cword[2], cword[3], cword[4],
								cword[5], cword[6], cword[7], cword[8], cword[9],
								cword[10], cword[11], cword[12], cword[13], cword[14]);
						}

						//apply error correction (it overwrites the buffer)
						decode_RS(&rs, (int8_t*)cword);

						//dump RS symbols again
						if(dump_rs)
						{
							printf(" ├ \033[93mCorrected RS symbols:\033[39m %02d %02d %02d %02d %02d %02d %02d %02d %02d | %02d %02d %02d %02d %02d %02d\n",
								cword[0], cword[1], cword[2], cword[3], cword[4],
								cword[5], cword[6], cword[7], cword[8], cword[9],
								cword[10], cword[11], cword[12], cword[13], cword[14]);
						}

						//descramble contents (raw_packet[] is not raw anymore :)
						for(uint8_t i=0; i<5; i++) raw_packet[3+i]^=scram[i];

						//extract the 30-bit timestamp into a 4-byte array
						uint8_t raw_timestamp[4]={	((raw_packet[3]<<1)&0x3F)|(raw_packet[4]>>7),
													(raw_packet[4]<<1)|(raw_packet[5]>>7),
													(raw_packet[5]<<1)|(raw_packet[6]>>7),
													(raw_packet[6]<<1)|(raw_packet[7]>>7)};

						//endianness swap
						uint8_t tmp;
						tmp=raw_timestamp[0]; raw_timestamp[0]=raw_timestamp[3]; raw_timestamp[3]=tmp;
						tmp=raw_timestamp[1]; raw_timestamp[1]=raw_timestamp[2]; raw_timestamp[2]=tmp;

						//convert the timestamp into seconds since 01-01-2000 (each tick is 3s)
						uint8_t tz=((raw_packet[7]>>4)&2)|((raw_packet[7]>>6)&1);
						*((uint32_t*)raw_timestamp)*=3;
						*((uint32_t*)raw_timestamp)+=3600*tz;

						//print the timestamp
						printf(" ├ \033[93mTimestamp:\033[39m %u\n", *((uint32_t*)raw_timestamp));
						time_t eczas = epoch+*((uint32_t*)raw_timestamp);

						//print decoded time
						printf(" ├ \033[93mDecoded:\033[39m %04d-%02d-%02d %02d:%02d:%02d (UTC+%d)\n",
							gmtime(&eczas)->tm_year+1900,
							gmtime(&eczas)->tm_mon+1,
							gmtime(&eczas)->tm_mday,
							gmtime(&eczas)->tm_hour,
							gmtime(&eczas)->tm_min,
							gmtime(&eczas)->tm_sec,
							tz);
						
						//print CRC
						printf(" └ \033[93mCRC:\033[39m");
						if(raw_packet[11]==calc_crc)
							printf(" \033[92mmatch\033[39m\n");
						else
							printf(" \033[91mmismatch\033[39m\n");
					}
					else //other packets
					{
						//print raw contents
						printf(" └ \033[93mRaw data:\033[39m ");
						for(uint8_t i=0; i<12; i++)
							printf("%02X ", raw_packet[i]);
						printf("\n");
					}
				}

				skip_samples=1;
				skip_cnt=0;
			}
		}
		else
		{
			skip_cnt++;
			if(skip_cnt==52*10) //skip 52 symbols (3.0-1.92=1.08; 1.08/0.02 is 54, we are adding some margin here)
			{
				skip_cnt=0;
				skip_samples=0;
			}
		}
	}

	return 0;
}
