#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

const int8_t sync[16]={-1, 1, -1, 1, -1, 1, -1, 1, -1, 1, -1, 1, -1, 1, -1, 1}; //sync symbol transitions
int16_t s[12*8*10+1]; //a whole 1.92s frame should fit in (50bps, 10 samples per symbol)
uint8_t skip_samples;
uint16_t skip_cnt;

uint8_t raw_packet[12];

uint8_t show_all=0; //show all frames (1) or time sync only (0)
const uint8_t scram[5]="\nGUM+";
const uint32_t epoch=946684800; //01-01-2000 00:00:00

int main(void)
{
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
					time_t now = time(NULL);
					struct tm* tm_now = localtime(&now);

					printf("[%02d:%02d:%02d] Raw data: ", tm_now->tm_hour, tm_now->tm_min, tm_now->tm_sec);
					for(uint8_t i=0; i<12; i++)
						printf("%02X ", raw_packet[i]);

					for(uint8_t i=0; i<5; i++) raw_packet[3+i]^=scram[i];

					uint8_t raw_timestamp[4]={	((raw_packet[3]<<1)&0x3F)|(raw_packet[4]>>7),
												(raw_packet[4]<<1)|(raw_packet[5]>>7),
												(raw_packet[5]<<1)|(raw_packet[6]>>7),
												(raw_packet[6]<<1)|(raw_packet[7]>>7)};

					//endianness swap
					uint8_t tmp;
					tmp=raw_timestamp[0]; raw_timestamp[0]=raw_timestamp[3]; raw_timestamp[3]=tmp;
					tmp=raw_timestamp[1]; raw_timestamp[1]=raw_timestamp[2]; raw_timestamp[2]=tmp;
					*((uint32_t*)raw_timestamp)*=3;
					printf("Timestamp: %u", *((uint32_t*)raw_timestamp));
					time_t eczas = epoch+*((uint32_t*)raw_timestamp);
					printf(" (%04d-%02d-%02d %02d:%02d:%02d)",
						localtime(&eczas)->tm_year+1900,
						localtime(&eczas)->tm_mon+1,
						localtime(&eczas)->tm_mday,
						localtime(&eczas)->tm_hour,
						localtime(&eczas)->tm_min,
						localtime(&eczas)->tm_sec);
					printf("\n");
				}

				skip_samples=1;
				skip_cnt=0;
			}
		}
		else
		{
			skip_cnt++;
			if(skip_cnt==12*8*10)
			{
				skip_cnt=0;
				skip_samples=0;
			}
		}
	}

	return 0;
}
