/* adrenc - An ADR (Astra Digital Radio) encoder                         */
/*=======================================================================*/
/* Copyright 2021 Philip Heron <phil@sanslogic.co.uk>                    */
/*                                                                       */
/* This program is free software: you can redistribute it and/or modify  */
/* it under the terms of the GNU General Public License as published by  */
/* the Free Software Foundation, either version 3 of the License, or     */
/* (at your option) any later version.                                   */
/*                                                                       */
/* This program is distributed in the hope that it will be useful,       */
/* but WITHOUT ANY WARRANTY; without even the implied warranty of        */
/* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         */
/* GNU General Public License for more details.                          */
/*                                                                       */
/* You should have received a copy of the GNU General Public License     */
/* along with this program.  If not, see <http://www.gnu.org/licenses/>. */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <twolame.h>

/* ADR uses 48kHz sample rate at 192kbit/s, producing a 576 byte frame */
#define ADR_FRAME_LEN 576

enum {
	_OPT_SCFCRC = 1000,
};

static const struct option _long_options[] = {
	{ "mode",           required_argument, 0, 'm'         },
	{ "station",        required_argument, 0, 's'         },
	{ "scfcrc",         no_argument,       0, _OPT_SCFCRC },
	{ 0,                0,                 0,  0          }
};

typedef struct {
	
	/* config */
	TWOLAME_MPEG_mode mode;
	uint8_t station_id[32];
	int scfcrc;
	
	/* twolame encoder */
	twolame_options *encopts;
	uint8_t mp2buffer[2][ADR_FRAME_LEN + 1];
	int frame;
	
	/* ADR ancillary data */
	char *cptr, cmsg[40]; /* The currently transmitting control message */
	int cindex; /* Index of the current message */
	char dc4_mode;
	
} _adrenc_t;

/* EBU Latin character set */
static const char *_charset[256] = {
	"","","","","","","","","","","","","","","","",
	"","","","","","","","","","","","","","","","",
	" ","!","\"","#","¤","%","&","'","(",")","*","+",",","-",".","/",
	"0","1","2","3","4","5","6","7","8","9",":",";","<","=",">","?",
	"@","A","B","C","D","E","F","G","H","I","J","K","L","M","N","O",
	"P","Q","R","S","T","U","V","W","X","Y","Z","[","\\","]","―","_",
	"‖","a","b","c","d","e","f","g","h","i","j","k","l","m","n","o",
	"p","q","r","s","t","u","v","w","x","y","z","{","|","}","¯","",
	"á","à","é","è","í","ì","ó","ò","ú","ù","Ñ","Ç","Ş","β","¡","Ĳ",
	"â","ä","ê","ë","î","ï","ô","ö","û","ü","ñ","ç","ş","ǧ","ı","ĳ",
	"ª","α","©","‰","Ǧ","ě","ň","ő","π","€","£","$","←","↑","→","↓",
	"º","¹","²","³","±","İ","ń","ű","µ","¿","÷","°","¼","½","¾","§",
	"Á","À","É","È","Í","Ì","Ó","Ò","Ú","Ù","Ř","Č","Š","Ž","Ð","Ŀ",
	"Â","Ä","Ê","Ë","Î","Ï","Ô","Ö","Û","Ü","ř","č","š","ž","đ","ŀ",
	"Ã","Å","Æ","Œ","ŷ","Ý","Õ","Ø","Þ","Ŋ","Ŕ","Ć","Ś","Ź","Ŧ","ð",
	"ã","å","æ","œ","ŵ","ý","õ","ø","þ","ŋ","ŕ","ć","ś","ź","ŧ","",
};

static uint32_t _utf8next(const char *str, const char **next)
{
	const uint8_t *c;
	uint32_t u, m;
	uint8_t b;
	
	/* Read and return a utf-8 character from str.
	 * If next is not NULL, it is pointed to the next
	 * character following this.
	 * 
	 * If an invalid code is detected, the function
	 * returns U+FFFD, � REPLACEMENT CHARACTER.
	*/
	
	c = (const uint8_t *) str;
	if(next) *next = str + 1;
	
	/* Shortcut for single byte codes */
	if(*c < 0x80) return(*c);
	
	/* Find the code length, initial bits and the first valid code */
	if((*c & 0xE0) == 0xC0) { u = *c & 0x1F; b = 1; m = 0x00080; }
	else if((*c & 0xF0) == 0xE0) { u = *c & 0x0F; b = 2; m = 0x00800; }
	else if((*c & 0xF8) == 0xF0) { u = *c & 0x07; b = 3; m = 0x10000; }
	else return(0xFFFD);
	
	while(b--)
	{
		/* All bytes after the first must begin 0x10xxxxxx */
		if((*(++c) & 0xC0) != 0x80) return(0xFFFD);
		
		/* Add the 6 new bits to the code */
		u = (u << 6) | (*c & 0x3F);
		
		/* Advance next pointer */
		if(next) (*next)++;
	}
	
	/* Reject overlong encoded characters */
	if(u < m) return(0xFFFD);
	
	return(u);
}

static void _encode_ebu_string(uint8_t *dst, const char *src, int len)
{
	uint32_t c;
	int i, j;
	
	for(i = 0; i < len && *src; i++)
	{
		c = _utf8next(src, &src);
		
		/* Lookup EBU character set for a match */
		for(j = 0; j < 256; j++)
		{
			if(c == _utf8next(_charset[j], NULL)) break;
		}
		
		/* Write character, or ' ' if not recognised */
		dst[i] = (j == 256 ? ' ' : j);
	}
	
	for(; i < len; i++)
	{
		dst[i] = '\0';
	}
}

void _decode_ebu_string(char *dst, const uint8_t *src, int len)
{
	int i;
	
	for(*dst = '\0', i = 0; *src && i < len; i++, src++)
	{
		strcat(dst, _charset[*src][0] ? _charset[*src] : "?");
	}
}

/* (7,4) Block Code lookup table */
static const uint8_t _74code[16] = {
	0x00,0x07,0x19,0x1E,0x2A,0x2D,0x33,0x34,0x4B,0x4C,0x52,0x55,0x61,0x66,0x78,0x7F,
};

static void _insert_adr_ancillary(_adrenc_t *s, uint8_t *data)
{
	uint8_t ad[18];
	uint8_t cw[36];
	int i, b;
	
	/* ADR ancillary data is built from 18 data bytes */
	memset(ad, 0, 18);
	
	/* Control data */
	for(i = 15; i < 18; i++)
	{
		if(*s->cptr == '\0')
		{
			uint8_t check;
			
			/* Generate the next message */
			switch(s->cindex++)
			{
			case 0: /* DC1 - Free-to-air service */
				snprintf(s->cmsg, 39, "\x02\x11\x04");
				break;
			
			case 1: /* DC4 - Program information */
				/* E1 = Extended Country Code */
				/* C  = Country Code */
				/* 2  = Coverage Area Code */
				/* 0A = Program Reference Number */
				/* %c = M/S/A/B = Mode */
				/* 2  = Program Category */
				snprintf(s->cmsg, 39, "\x02\x14" "E1C20A%c2" "\x04", s->dc4_mode);
				break;
			
			case 2: /* SYN - Station ID information */
				snprintf(s->cmsg, 39, "\x02\x16" "%s#" "\x04", s->station_id);
				s->cindex = 0;
				break;
			}
			
			/* Calculate the checksum */
			for(s->cptr = s->cmsg, check = 0x00; *s->cptr; s->cptr++)
			{
				check += *s->cptr & 0x7F;
			}
			
			/* Append the checksum and end of message byte */
			snprintf(s->cptr, 4, "%1X%1X\x03", check & 0x0F, check >> 4);
			
			/* Reset the pointer */
			s->cptr = s->cmsg;
		}
		
		ad[i] = *s->cptr++;
	}
	
	/* Control flags (MSBs of the 3 control data bytes) */
	ad[15] |= 0 << 7; /* 1 == Start of key period for pay service smart card decryption */
	ad[16] |= 0 << 7; /* 1 == RDS data and auxiliary data are complemented in this frame */
	ad[17] |= s->scfcrc << 7; /* 1 == Scale factor CRC is present */
	
	/* Generate the 36 codewords */
	for(i = 0; i < 18; i++)
	{
		cw[i * 2 + 0] = _74code[ad[i] & 0x0F];
		cw[i * 2 + 1] = _74code[ad[i] >> 4];
	}
	
	/* Write the codewords into the ancillary data, interleaved 36x7 */
	data += 0x21C;
	for(i = 0; i < 252; i++)
	{
		/* Writing to byte b, skipping the SCF-CRC */
		b = (i >> 3);
		if(b >= 30) b += 4;
		
		data[b] |= ((cw[i % 36] >> (i / 36)) & 1) << (7 - (i & 7));
	}
}

static uint8_t *_encode_adr_frame(_adrenc_t *s, const int16_t *pcm)
{
	int r;
	uint8_t *fn = s->mp2buffer[(s->frame + 1) & 1];
	uint8_t *fl = s->mp2buffer[(s->frame + 0) & 1];
	uint8_t *mp2 = NULL;
	
	if(pcm)
	{
		r = twolame_encode_buffer_interleaved(s->encopts, pcm, TWOLAME_SAMPLES_PER_FRAME, fn, ADR_FRAME_LEN + 1);
	}
	else
	{
		r = twolame_encode_flush(s->encopts, fn, ADR_FRAME_LEN + 1);
	}
	
	if(r > 0)
	{
		_insert_adr_ancillary(s, fn);
		
		if(s->scfcrc == 0)
		{
			mp2 = fn;
		}
		else if(s->frame > 0)
		{
			/* The ScF CRC of each frame is stored in the previous frame */
			twolame_set_DAB_scf_crc(s->encopts, fl, r);
			mp2 = fl;
		}
		
		s->frame++;
	}
	
	return(mp2);
}

static void print_usage(void)
{
	printf(
		"\n"
		"Usage: adrenc [options] input output\n"
		"\n"
		"  -m, --mode <name>             Set the channel mode (mono|dual|joint|stereo).\n"
		"                                Default: joint\n"
		"  -s, --station <id>            Set the station ID. Default: \"\"\n"
		"                                Limited to 32 characters, can't contain a '#'.\n"
		"      --scfcrc                  Enable Scale Factor CRC (ScF-CRC).\n"
		"\n"
	);
}

static void _print_summary(_adrenc_t *s)
{
	const char *str;
	char id[33];
	
	switch(s->mode)
	{
	case TWOLAME_MONO: str = "Mono"; break;
	case TWOLAME_DUAL_CHANNEL: str = "Dual"; break;
	case TWOLAME_JOINT_STEREO: str = "Joint Stereo"; break;
	case TWOLAME_STEREO: str = "Stereo"; break;
	default: str = "Unknown"; break;
	}
	fprintf(stderr, "Mode: %s\n", str);
	
	_decode_ebu_string(id, s->station_id, 32);
	fprintf(stderr, "Station ID: '%s'\n", id);
	
	if(s->scfcrc)
	{
		fprintf(stderr, "ScF-CRC enabled\n");
	}
}

int main(int argc, char *argv[])
{
	int c;
	int option_index;
	int16_t pcm[TWOLAME_SAMPLES_PER_FRAME * 2];
	int i, r;
	FILE *fpcm, *fmp2;
	_adrenc_t s;
	uint8_t *mp2;
	
	memset(&s, 0, sizeof(_adrenc_t));
	
	s.mode = TWOLAME_JOINT_STEREO;
	s.dc4_mode = 'S';
	
	s.cmsg[0] = '\0';
	s.cptr = s.cmsg;
	s.cindex = 0;
	
	opterr = 0;
	while((c = getopt_long(argc, argv, "m:s:", _long_options, &option_index)) != -1)
	{
		switch(c)
		{
		case 'm': /* -m, --mode <mono|dual|joint|stereo> */
			
			if(strcmp("mono", optarg) == 0) { s.mode = TWOLAME_MONO; s.dc4_mode = 'M'; }
			else if(strcmp("dual", optarg) == 0) { s.mode = TWOLAME_DUAL_CHANNEL; s.dc4_mode = 'A'; }
			else if(strcmp("joint", optarg) == 0) { s.mode = TWOLAME_JOINT_STEREO; s.dc4_mode = 'S'; }
			else if(strcmp("stereo", optarg) == 0) { s.mode = TWOLAME_STEREO; s.dc4_mode = 'S'; }
			else
			{
				fprintf(stderr, "Unrecognised mode '%s'\n", optarg);
				return(-1);
			}
			break;
		
		case 's': /* -s, --station <id> */
			
			_encode_ebu_string(s.station_id, optarg, 32);
			break;
		
		case _OPT_SCFCRC:
			s.scfcrc = 1;
			break;
		
		case '?':
			print_usage();
			return(0);
		}
	}
	
	if(argc - optind != 2)
	{
		print_usage();
		return(-1);
	}
	
	if(strcmp(argv[optind + 0], "-") == 0)
	{
		fpcm = stdin;
	}
	else
	{
		fpcm = fopen(argv[optind + 0], "rb");
		if(!fpcm)
		{
			perror(argv[optind + 0]);
			return(-1);
		}
	}
	
	if(strcmp(argv[optind + 1], "-") == 0)
	{
		fmp2 = stdout;
	}
	else
	{
		fmp2 = fopen(argv[optind + 1], "wb");
		if(!fmp2)
		{
			perror(argv[optind + 1]);
			return(-1);
		}
	}
	
	_print_summary(&s);
	
	/* Configure twolame */
	s.encopts = twolame_init();
	twolame_set_in_samplerate(s.encopts, 48000);
	twolame_set_out_samplerate(s.encopts, 48000);
	twolame_set_bitrate(s.encopts, 192);
	twolame_set_num_channels(s.encopts, s.mode == TWOLAME_MONO ? 1 : 2);
	twolame_set_mode(s.encopts, s.mode);
	twolame_set_error_protection(s.encopts, TRUE);
	twolame_set_num_ancillary_bits(s.encopts, 36 * 8);
	if(s.scfcrc)
	{
		twolame_set_DAB(s.encopts, TRUE);
		twolame_set_DAB_scf_crc_length(s.encopts);
	}
	r = twolame_init_params(s.encopts);
	if(r != 0) return(r);
	
	/* Encode the audio */
	while((i = fread(pcm, sizeof(int16_t) * TWOLAME_SAMPLES_PER_FRAME * (s.mode == TWOLAME_MONO ? 1 : 2), 1, fpcm)) > 0)
	{
		mp2 = _encode_adr_frame(&s, pcm);
		if(mp2) fwrite(mp2, 1, ADR_FRAME_LEN, fmp2);
	}
	
	/* Flush the final packet */
	mp2 = _encode_adr_frame(&s, NULL);
	if(mp2) fwrite(mp2, 1, ADR_FRAME_LEN, fmp2);
	
	fprintf(stderr, "Encoded %d frames.\n", s.frame);
	
	/* Tidy up */
	twolame_close(&s.encopts);
	
	if(fmp2 != stdout) fclose(fmp2);
	if(fpcm != stdin) fclose(fpcm);
	
	return(0);
}

