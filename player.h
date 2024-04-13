/*
 *  player.h - TMS 9919 sound simulation
 *
 * Copyright (c) 2023 Pete Eberlein
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#define SCALE 8
//#define SHIFT_RESET  0x8000
#define SHIFT_RESET  0x4000
#define FEEDBACK_PATTERN 0x9
#define NO_ANTIALIAS -2147483648
#define CLOCK_3_58MHZ 3579545
//#define SAMPLE_FREQUENCY 44100
#define SAMPLE_FREQUENCY 48000
#define PERIODIC_NOISE_CYCLE 15
#define CPU_CLK_FREQ 3000000

static int reg[] = {1,0xf,1,0xf,1,0xf,1,0xf};
static int regLatch = 0;
static int freqCounter[] = {0,0,0,0};
static int freqPos[] = {NO_ANTIALIAS,NO_ANTIALIAS,NO_ANTIALIAS};
static int noiseFreq = 0x10;
static int noiseShiftReg = SHIFT_RESET;
static int freqPolarity[] = {1,1,1,1};
//static int PSG_VOLUME[] = {25, 20, 16, 13, 10, 8, 6, 5, 4, 3, 3, 2, 2, 1, 1, 0};
static double PSG_VOLUME[] = {25, 19.858206, 15.773934, 12.529681,
	9.952679, 7.905694, 6.279716, 4.988156, 3.962233, 3.147314,
	2.500000, 1.985821, 1.577393, 1.252968, 0.995268, 0};
static int audioIn = 0; // from the CRU 

static int clk = (CLOCK_3_58MHZ << SCALE) / 16 / SAMPLE_FREQUENCY;
static int clkFrac = 0;

static int dump_soundlist = 0;
static unsigned char row[32];
static int row_len = 0;
static int duration = 0;

// this determines the max number of bytes writable to the sound chip
// during the duration of a audio frame (1024 samples or approx 21 ms at 48kHz)
#define FIFO_SIZE 1024
static unsigned char fifo_data[FIFO_SIZE] = {};
static unsigned char fifo_gate[FIFO_SIZE] = {}; // 0=9199 1=audio gate
static unsigned int fifo_timestamp[FIFO_SIZE] = {};
static unsigned int fifo_count = 0; // current fullness of the fifo

static void snd_fifo(unsigned char value, unsigned char gate, unsigned int timestamp)
{
	if (fifo_count >= FIFO_SIZE) return;
	fifo_data[fifo_count] = value;
	fifo_gate[fifo_count] = gate;
	fifo_timestamp[fifo_count] = timestamp;
	fifo_count++;
}

static void snd(unsigned char value)
{
	if (dump_soundlist) {
		row[row_len++] = value;
	}

	if (value & 0x80) {
		regLatch = (value >> 4) & 7;
		reg[regLatch] = (reg[regLatch] & 0x3f0) | (value & 0x0f);
	} else {
		if (regLatch == 0 || regLatch == 2 || regLatch == 4) {
			reg[regLatch] = ((value & 0x3f) << 4) |
			                (reg[regLatch] & 0x0f);
		} else {
			reg[regLatch] = value & 0x0f;  // noise, or gain
		}
	}
	//fprintf(stderr, "%d %x\n", regLatch, reg[regLatch]);
	switch(regLatch) {
	case 0:
	case 2:
	case 4:
		if (reg[regLatch] == 0)
			reg[regLatch] = 0x400;
		break;
	case 6:
		noiseFreq = 0x10 << (reg[6] & 3);
		noiseShiftReg = SHIFT_RESET;
		break;
	}
}

#if 0
// this doesn't work with data fifo
void mute(int en)
{
	static int save[4];
	static int cur = 0;
	if (en == cur) return;
	cur = en;
	if (en) {
		save[0] = reg[1];
		save[1] = reg[3];
		save[2] = reg[5];
		save[3] = reg[7];
		reg[1] = 0xf;
		reg[3] = 0xf;
		reg[5] = 0xf;
		reg[7] = 0xf;
	} else {
		reg[1] = save[0];
		reg[3] = save[1];
		reg[5] = save[2];
		reg[7] = save[3];
	}

}
#endif

// process one byte from the data fifo
static void play_fifo(void)
{
	snd(fifo_data[0]);
	fifo_count--;
	memmove(fifo_data, fifo_data+1, sizeof(fifo_data[0]) * fifo_count);
	memmove(fifo_timestamp, fifo_timestamp+1, sizeof(fifo_timestamp[0]) * fifo_count);
}

// get the next timestamp from the fifo and scale it from cpu cycles to sound chip cycles
static void next_fifo(unsigned int *next,
		unsigned int last_cpu_cycles, unsigned int current_cpu_cycles,
		unsigned int ticks)
{
	*next = 0;
	while (fifo_count > 0 && current_cpu_cycles != last_cpu_cycles) {
		unsigned int ts = fifo_timestamp[0];
		int rel = (int)(ts - last_cpu_cycles);
		if (rel > 0) {
			*next = rel * ticks / (current_cpu_cycles - last_cpu_cycles);
			break;
		}
		play_fifo();
	}
}


#if 1
static void update(unsigned char *buffer, int offset, int samplesToGenerate, unsigned int current_cpu_cycles)
{
	int sample = 0, i = 0;
	static double d = 0.0, v = 0.0;
	static int enable = 0xf;
	static unsigned int last_cpu_cycles = 0;
	unsigned int ticks = (unsigned int)samplesToGenerate * CLOCK_3_58MHZ / (SAMPLE_FREQUENCY * 16);
	unsigned int next = 0;
	unsigned int n = 0;

	//printf("%d\n", (int)(current_cpu_cycles-last_cycles[9]));
	//memmove(last_cycles, last_cycles+1, sizeof(last_cycles)-sizeof(last_cycles[0]));
	//last_cycles[9] = current_cpu_cycles;

	// Using a fixed delta of cpu cycles seems to sound better than variable timing
	last_cpu_cycles = current_cpu_cycles - ((unsigned int)CPU_CLK_FREQ * samplesToGenerate / SAMPLE_FREQUENCY);
	//printf("%u %u %d %d\n", last_cpu_cycles, current_cpu_cycles, current_cpu_cycles - last_cpu_cycles, fifo_count);

	// process audio data between last_cpu_cycles and current_cpu_cycles
	next_fifo(&next, last_cpu_cycles, current_cpu_cycles, ticks);

	// When samplesToGenerate is 1024, n will be 4772 or 4773
	// SAMPLE_FREQUENCY is 48000Hz, time is 0.021333333s
	// CLOCK_3_58MHZ is 3579545Hz

	while (samplesToGenerate) {
		double scale = 1.0;

		n++;
		if (next == n) {
			play_fifo();
			next_fifo(&next, last_cpu_cycles, current_cpu_cycles, ticks);
		}

		d += SAMPLE_FREQUENCY;
		if (d > CLOCK_3_58MHZ/16.0) {
			d -= CLOCK_3_58MHZ/16.0;
			scale = (SAMPLE_FREQUENCY - d) / SAMPLE_FREQUENCY;
		}

		for (i = 0; i < 3; i++) {
			if (enable & (1 << i))
				v += PSG_VOLUME[reg[i*2+1]] * freqPolarity[i] * scale;
		}
		if (enable & (1<<3))
			v += PSG_VOLUME[reg[7]] * ((noiseShiftReg & 1) ? -1 : 0) * scale;

		if (scale != 1.0) {
			static double last = 0.0;

			last = .8*last + .2*v; // exponential low-pass filter
			buffer[offset++] = last*0.25+128;
			samplesToGenerate--;

			v = 0.0;
			scale = 1.0 - scale;
			for (i = 0; i < 3; i++) {
				if (enable & (1 << i))
					v += PSG_VOLUME[reg[i*2+1]] * freqPolarity[i] * scale;
			}
			if (enable & (1<<3))
				v += PSG_VOLUME[reg[7]] * ((noiseShiftReg & 1) ? -1 : 0) * scale;
		}

		freqCounter[0] -= 1;
		freqCounter[1] -= 1;
		freqCounter[2] -= 1;
		if (noiseFreq == 0x80)
			freqCounter[3] = freqCounter[2];
		else
			freqCounter[3] -= 1;

		for (i = 0; i < 3; i++) {
			int counter = freqCounter[i];
			if (counter <= 0) {
				int tone = reg[i<<1];
				freqPolarity[i] = 1-freqPolarity[i];
				freqCounter[i] += tone;
			}
		}
		if (freqCounter[3] <= 0) {
			freqPolarity[3] *= -1;
			if (noiseFreq != 0x80) {
				freqCounter[3] += noiseFreq;
			}
			if (freqPolarity[3] == 1) {
				int feedback;

				if (reg[6] & 0x04) { // white noise
					feedback = (noiseShiftReg & FEEDBACK_PATTERN) &&
						  ((noiseShiftReg & FEEDBACK_PATTERN) ^ FEEDBACK_PATTERN);
					//feedback = (noiseShiftReg ^ (noiseShiftReg >> 3)) & 1;
					feedback <<= 1; // twice as long as periodic noise cycle

				} else { // periodic noise
					feedback = noiseShiftReg & 1;
				}
				noiseShiftReg = (noiseShiftReg >> 1) | (feedback << (PERIODIC_NOISE_CYCLE-1));
				//fprintf(stderr, "%04x\n", noiseShiftReg);
			}
		}
	}
	last_cpu_cycles = current_cpu_cycles;
}

#else
static void update(signed char *buffer, int offset, int samplesToGenerate)
{
	int sample, i;
	int outputChannel[4];
	for (sample = 0; sample < samplesToGenerate; sample++) {
		for (i = 0; i < 3; i++) {
			if (freqPos[i] != NO_ANTIALIAS)
				outputChannel[i] = (PSG_VOLUME[reg[i*2+1]] * freqPos[i]) >> SCALE;
			else
				outputChannel[i] = PSG_VOLUME[reg[i*2+1]] * freqPolarity[i];
		}
		outputChannel[3] = PSG_VOLUME[reg[7]] * (noiseShiftReg & 1) * 2;

		int output = outputChannel[0] + outputChannel[1] + outputChannel[2] + outputChannel[3];
		if (output > 0x7f) output = 0x7f;
		else if (output < -0x80) output = -0x80;

		buffer[offset + sample] = output+128;

		clkFrac += clk;
		int clkCycles = clkFrac >> SCALE;
		int clkCyclesScaled = clkCycles << SCALE;
		clkFrac -= clkCyclesScaled;

		freqCounter[0] -= clkCycles;
		freqCounter[1] -= clkCycles;
		freqCounter[2] -= clkCycles;
		if (noiseFreq == 0x80)
			freqCounter[3] = freqCounter[2];
		else
			freqCounter[3] -= clkCycles;

		for (i = 0; i < 3; i++) {
			int counter = freqCounter[i];
			if (counter <= 0) {
				int tone = reg[i<<1];
				if (tone > 6) {
					freqPos[i] = ((clkCyclesScaled - clkFrac + (2 << SCALE) * counter) << SCALE) *
							freqPolarity[i] / (clkCyclesScaled + clkFrac);
					freqPolarity[i] *= -1;
				} else {
					freqPolarity[i] = 1;
					freqPos[i] = NO_ANTIALIAS;
				}
				freqCounter[i] += tone * (clkCycles / tone + 1);
			} else {
				freqPos[i] = NO_ANTIALIAS;
			}
		}
		if (freqCounter[3] <= 0) {
			freqPolarity[3] *= -1;
			if (noiseFreq != 0x80)
				freqCounter[3] += noiseFreq * (clkCycles / noiseFreq + 1);
			if (freqPolarity[3] == 1) {
				int feedback;

				if (reg[6] & 0x04) { // white noise
					feedback = (noiseShiftReg & FEEDBACK_PATTERN) != 0 &&
							((noiseShiftReg & FEEDBACK_PATTERN) ^ FEEDBACK_PATTERN) != 0
							? 2 : 0;
					//feedback = ((noiseShiftReg >> 4) ^ (noiseShiftReg)) & 1;


				} else { // periodic noise
					feedback = noiseShiftReg & 1;
				}
				noiseShiftReg = (noiseShiftReg >> 1) | (feedback << (PERIODIC_NOISE_CYCLE-1));
				//fprintf(stderr, "%04x\n", noiseShiftReg);


				static int count = 0;
				count++;
				if (noiseShiftReg == 1) {
					fprintf(stderr, "count=%d\n", count);
					count = 0;
				}

			}
		}
	}
}
#endif


static void vsync()
{
	static int i = 0;

	if (dump_soundlist) {
		if (row_len == 0) {
			duration++;
		} else {
			if (duration) putchar(duration);
			duration = 1;
			putchar(row_len);
			fwrite(row, 1, row_len, stdout);
			row_len = 0;
		}
		return;
	}

	if (i == 0) {
		struct {
			unsigned int ChunkID;
			unsigned int ChunkSize;
			unsigned int Format;
			unsigned int Subchunk1ID;
			unsigned int Subchunk1Size;
			unsigned short AudioFormat;
			unsigned short NumChannels;
			unsigned int SampleRate;
			unsigned int ByteRate;
			unsigned short BlockAlign;
			unsigned short BitsPerSample;
			unsigned int Subchunk2ID;
			unsigned int Subchunk2Size;
		} wavhdr = {
			0x46464952,
			0x00018fcc,
			0x45564157,
			0x20746d66,
			0x10,
			1, // AudioFormat
			1, // NumChannels,
			SAMPLE_FREQUENCY,
			0x00015888,
			1, // blockalign
			8, // Bitspersample
			0x61746164,
			0xffffffff,//800*30
		};
		fwrite(&wavhdr, sizeof(wavhdr), 1, stdout);
	}
	
	
	unsigned char buffer[SAMPLE_FREQUENCY * 100 / 5994];
	update(buffer, 0, sizeof(buffer), 0);
	fwrite(buffer, sizeof(buffer), 1, stdout);
	
	fprintf(stderr, "%d %03x/%x %03x/%x %03x/%x %03x/%x\n", i,
		reg[0], reg[1],
		reg[2], reg[3],
		reg[4], reg[5],
		reg[6], reg[7]);
	i++;
}

static void terminate_soundlist()
{
	if (dump_soundlist)
		putchar(0); // terminator
}


