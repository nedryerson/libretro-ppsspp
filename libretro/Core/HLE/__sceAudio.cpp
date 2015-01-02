// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include "base/mutex.h"

#include "Globals.h" // only for clamp_s16
#include "Common/CommonTypes.h"
#include "Common/ChunkFile.h"
#include "Common/FixedSizeQueue.h"
#include "Common/Atomics.h"

#ifdef _M_SSE
#include <emmintrin.h>
#endif

#include "Core/CoreTiming.h"
#include "Core/MemMap.h"
#include "Core/Host.h"
#include "Core/Config.h"
#include "Core/HLE/__sceAudio.h"
#include "Core/HLE/sceAudio.h"
#include "Core/HLE/sceKernel.h"
#include "Core/HLE/sceKernelThread.h"


enum latency {
	LOW_LATENCY = 0,
	MEDIUM_LATENCY = 1,
	HIGH_LATENCY = 2,
};

int eventAudioUpdate = -1;
int eventHostAudioUpdate = -1; 
int mixFrequency = 44100;

const int hwSampleRate = 44100;

int hwBlockSize = 64;
int hostAttemptBlockSize = 512;

static int audioIntervalCycles;
static int audioHostIntervalCycles;

// High and low watermarks, basically.  For perfect emulation, the correct values are 0 and 1, respectively.
// TODO: Tweak. Hm, there aren't actually even used currently...
static int chanQueueMaxSizeFactor;
static int chanQueueMinSizeFactor;

#include "FixedSizeQueueC.c"

static inline s16 adjustvolume(s16 sample, int vol) {
#ifdef ARM
	register int r;
	asm volatile("smulwb %0, %1, %2\n\t" \
	             "ssat %0, #16, %0" \
	             : "=r"(r) : "r"(vol), "r"(sample));
	return r;
#else
	return clamp_s16((sample * vol) >> 16);
#endif
}

inline void AdjustVolumeBlock(s16 *out, s16 *in, size_t size, int leftVol, int rightVol) {
#ifdef _M_SSE
	if (leftVol <= 0x7fff && rightVol <= 0x7fff) {
		__m128i volume = _mm_set_epi16(leftVol, rightVol, leftVol, rightVol, leftVol, rightVol, leftVol, rightVol);
		while (size >= 16) {
			__m128i indata1 = _mm_loadu_si128((__m128i *)in);
			__m128i indata2 = _mm_loadu_si128((__m128i *)(in + 8));
			_mm_storeu_si128((__m128i *)out, _mm_mulhi_epi16(indata1, volume));
			_mm_storeu_si128((__m128i *)(out + 8), _mm_mulhi_epi16(indata2, volume));
			in += 16;
			out += 16;
			size -= 16;
		}
	} else {
		// We have to shift inside the loop to avoid the signed multiply issue.
		leftVol >>= 1;
		rightVol >>= 1;
		__m128i volume = _mm_set_epi16(leftVol, rightVol, leftVol, rightVol, leftVol, rightVol, leftVol, rightVol);
		while (size >= 16) {
			__m128i indata1 = _mm_loadu_si128((__m128i *)in);
			__m128i indata2 = _mm_loadu_si128((__m128i *)(in + 8));
			_mm_storeu_si128((__m128i *)out, _mm_slli_epi16(_mm_mulhi_epi16(indata1, volume), 1));
			_mm_storeu_si128((__m128i *)(out + 8), _mm_slli_epi16(_mm_mulhi_epi16(indata2, volume), 1));
			in += 16;
			out += 16;
			size -= 16;
		}
	}
#endif
	for (size_t i = 0; i < size; i += 2) {
		out[i] = adjustvolume(in[i], leftVol);
		out[i + 1] = adjustvolume(in[i + 1], rightVol);
	}
}

static void hleAudioUpdate(u64 userdata, int cyclesLate) {
	// Schedule the next cycle first.  __AudioUpdate() may consume cycles.
	CoreTiming::ScheduleEvent(audioIntervalCycles - cyclesLate, eventAudioUpdate, 0);

	__AudioUpdate();
}

static void hleHostAudioUpdate(u64 userdata, int cyclesLate) {
	CoreTiming::ScheduleEvent(audioHostIntervalCycles - cyclesLate, eventHostAudioUpdate, 0);

	// Not all hosts need this call to poke their audio system once in a while, but those that don't
	// can just ignore it.
	host->UpdateSound();
}

static void __AudioCPUMHzChange() {
	audioIntervalCycles = (int)(usToCycles(1000000ULL) * hwBlockSize / hwSampleRate);
	audioHostIntervalCycles = (int)(usToCycles(1000000ULL) * hostAttemptBlockSize / hwSampleRate);
}


void __AudioInit() {
	mixFrequency = 44100;

	switch (g_Config.iAudioLatency) {
	case LOW_LATENCY:
		chanQueueMaxSizeFactor = 1;
		chanQueueMinSizeFactor = 1;
		hwBlockSize = 16;
		hostAttemptBlockSize = 256;
		break;
	case MEDIUM_LATENCY:
		chanQueueMaxSizeFactor = 2;
		chanQueueMinSizeFactor = 1;
		hwBlockSize = 64;
		hostAttemptBlockSize = 512;
		break;
	case HIGH_LATENCY:
		chanQueueMaxSizeFactor = 4;
		chanQueueMinSizeFactor = 2;
		hwBlockSize = 64;
		hostAttemptBlockSize = 512;
		break;

	}

	__AudioCPUMHzChange();

	eventAudioUpdate = CoreTiming::RegisterEvent("AudioUpdate", &hleAudioUpdate);
	eventHostAudioUpdate = CoreTiming::RegisterEvent("AudioUpdateHost", &hleHostAudioUpdate);

	CoreTiming::ScheduleEvent(audioIntervalCycles, eventAudioUpdate, 0);
	CoreTiming::ScheduleEvent(audioHostIntervalCycles, eventHostAudioUpdate, 0);
	for (u32 i = 0; i < PSP_AUDIO_CHANNEL_MAX + 1; i++)
		chans[i].clear();

	mixBuffer = new s32[hwBlockSize * 2];
	memset(mixBuffer, 0, hwBlockSize * 2 * sizeof(s32));

   queue_clear();
	CoreTiming::RegisterMHzChangeCallback(&__AudioCPUMHzChange);
}

void __AudioDoState(PointerWrap &p) {
	auto s = p.Section("sceAudio", 1);
	if (!s)
		return;

	p.Do(eventAudioUpdate);
	CoreTiming::RestoreRegisterEvent(eventAudioUpdate, "AudioUpdate", &hleAudioUpdate);
	p.Do(eventHostAudioUpdate);
	CoreTiming::RestoreRegisterEvent(eventHostAudioUpdate, "AudioUpdateHost", &hleHostAudioUpdate);

	p.Do(mixFrequency);

	{	
      queue_DoState(p);
	}

	int chanCount = ARRAY_SIZE(chans);
	p.Do(chanCount);
	if (chanCount != ARRAY_SIZE(chans))
	{
		ERROR_LOG(SCEAUDIO, "Savestate failure: different number of audio channels.");
		return;
	}
	for (int i = 0; i < chanCount; ++i)
		chans[i].DoState(p);

	__AudioCPUMHzChange();
}

void __AudioShutdown() {
	delete [] mixBuffer;

	mixBuffer = 0;
	for (u32 i = 0; i < PSP_AUDIO_CHANNEL_MAX + 1; i++)
		chans[i].clear();
}

u32 __AudioEnqueue(AudioChannel &chan, int chanNum, bool blocking)
{
	u32 ret = chan.sampleCount;

	if (chan.sampleAddress == 0)
   {
		// For some reason, multichannel audio lies and returns the sample count here.
		if (chanNum == PSP_AUDIO_CHANNEL_SRC || chanNum == PSP_AUDIO_CHANNEL_OUTPUT2)
			ret = 0;
	}

	// If there's anything on the queue at all, it should be busy, but we try to be a bit lax.
	//if (chan.sampleQueue.size() > chan.sampleCount * 2 * chanQueueMaxSizeFactor || chan.sampleAddress == 0) {
   
	if (chan.sampleQueue.size() > 0)
   {
		if (!blocking) // Non-blocking doesn't even enqueue, but it's not commonly used.
         return SCE_ERROR_AUDIO_CHANNEL_BUSY;
      {
			// TODO: Regular multichannel audio seems to block for 64 samples less?  Or enqueue the first 64 sync?
			int blockSamples = (int)chan.sampleQueue.size() / 2 / chanQueueMinSizeFactor;

			if (__KernelIsDispatchEnabled())
         {
				AudioChannelWaitInfo waitInfo = {__KernelGetCurThread(), blockSamples};
				chan.waitingThreads.push_back(waitInfo);
				// Also remember the value to return in the waitValue.
				__KernelWaitCurThread(WAITTYPE_AUDIOCHANNEL, (SceUID)chanNum + 1, ret, 0, false, "blocking audio");
			}
         else // TODO: Maybe we shouldn't take this audio after all?
				ret = SCE_KERNEL_ERROR_CAN_NOT_WAIT;

			// Fall through to the sample queueing, don't want to lose the samples even though
			// we're getting full.  The PSP would enqueue after blocking.
		}
	}

	if (chan.sampleAddress == 0)
		return ret;

	int leftVol = chan.leftVolume;
	int rightVol = chan.rightVolume;

	if (leftVol == (1 << 15) && rightVol == (1 << 15) && chan.format == PSP_AUDIO_FORMAT_STEREO && IS_LITTLE_ENDIAN)
   {
		// TODO: Add mono->stereo conversion to this path.

		// Good news: the volume doesn't affect the values at all.
		// We can just do a direct memory copy.
		const u32 totalSamples = chan.sampleCount * (chan.format == PSP_AUDIO_FORMAT_STEREO ? 2 : 1);
		s16 *buf1 = 0, *buf2 = 0;
		size_t sz1, sz2;
		chan.sampleQueue.pushPointers(totalSamples, &buf1, &sz1, &buf2, &sz2);

		if (Memory::IsValidAddress(chan.sampleAddress + (totalSamples - 1) * sizeof(s16_le)))
      {
			Memory::MemcpyUnchecked(buf1, chan.sampleAddress, (u32)sz1 * sizeof(s16));
			if (buf2)
				Memory::MemcpyUnchecked(buf2, chan.sampleAddress + (u32)sz1 * sizeof(s16), (u32)sz2 * sizeof(s16));
		}
	}
   else
   {
		// Remember that maximum volume allowed is 0xFFFFF so left shift is no issue.
		// This way we can optimally shift by 16.
		leftVol <<=1;
		rightVol <<=1;

		if (chan.format == PSP_AUDIO_FORMAT_STEREO)
      {
			const u32 totalSamples = chan.sampleCount * 2;

			s16_le *sampleData = (s16_le *) Memory::GetPointer(chan.sampleAddress);

			// Walking a pointer for speed.  But let's make sure we wouldn't trip on an invalid ptr.
			if (Memory::IsValidAddress(chan.sampleAddress + (totalSamples - 1) * sizeof(s16_le)))
         {
				s16 *buf1 = 0, *buf2 = 0;
				size_t sz1, sz2;
				chan.sampleQueue.pushPointers(totalSamples, &buf1, &sz1, &buf2, &sz2);
				AdjustVolumeBlock(buf1, sampleData, sz1, leftVol, rightVol);
				if (buf2)
					AdjustVolumeBlock(buf2, sampleData + sz1, sz2, leftVol, rightVol);
			}
		}
      else if (chan.format == PSP_AUDIO_FORMAT_MONO)
      {
			// Rare, so unoptimized. Expands to stereo.
			for (u32 i = 0; i < chan.sampleCount; i++)
         {
				s16 sample = (s16)Memory::Read_U16(chan.sampleAddress + 2 * i);
				chan.sampleQueue.push(adjustvolume(sample, leftVol));
				chan.sampleQueue.push(adjustvolume(sample, rightVol));
			}
		}
	}
	return ret;
}

inline void __AudioWakeThreads(AudioChannel &chan, int result, int step) {
	u32 error;
	bool wokeThreads = false;
	for (size_t w = 0; w < chan.waitingThreads.size(); ++w) {
		AudioChannelWaitInfo &waitInfo = chan.waitingThreads[w];
		waitInfo.numSamples -= step;

		// If it's done (there will still be samples on queue) and actually still waiting, wake it up.
		u32 waitID = __KernelGetWaitID(waitInfo.threadID, WAITTYPE_AUDIOCHANNEL, error);
		if (waitInfo.numSamples <= 0 && waitID != 0) {
			// DEBUG_LOG(SCEAUDIO, "Woke thread %i for some buffer filling", waitingThread);
			u32 ret = result == 0 ? __KernelGetWaitValue(waitInfo.threadID, error) : SCE_ERROR_AUDIO_CHANNEL_NOT_RESERVED;
			__KernelResumeThreadFromWait(waitInfo.threadID, ret);
			wokeThreads = true;

			chan.waitingThreads.erase(chan.waitingThreads.begin() + w--);
		}
		// This means the thread stopped waiting, so stop trying to wake it.
		else if (waitID == 0)
			chan.waitingThreads.erase(chan.waitingThreads.begin() + w--);
	}

	if (wokeThreads) {
		__KernelReSchedule("audio drain");
	}
}

void __AudioWakeThreads(AudioChannel &chan, int result) {
	__AudioWakeThreads(chan, result, 0x7FFFFFFF);
}

void __AudioSetOutputFrequency(int freq) {
	WARN_LOG(SCEAUDIO, "Switching audio frequency to %i", freq);
	mixFrequency = freq;
}

inline void ClampBufferToS16(s16 *out, s32 *in, size_t size) {
#ifdef _M_SSE
	// Size will always be 16-byte aligned as the hwBlockSize is.
	while (size >= 8) {
		__m128i in1 = _mm_loadu_si128((__m128i *)in);
		__m128i in2 = _mm_loadu_si128((__m128i *)(in + 4));
		__m128i packed = _mm_packs_epi32(in1, in2);
		_mm_storeu_si128((__m128i *)out, packed);
		out += 8;
		in += 8;
		size -= 8;
	}
#endif
	for (size_t i = 0; i < size; i++)
		out[i] = clamp_s16(in[i]);
}

// Mix samples from the various audio channels into a single sample queue.
// This single sample queue is where __AudioMix should read from. If the sample queue is full, we should
// just sleep the main emulator thread a little.
void __AudioUpdate()
{
	// Audio throttle doesn't really work on the PSP since the mixing intervals are so closely tied
	// to the CPU. Much better to throttle the frame rate on frame display and just throw away audio
	// if the buffer somehow gets full.
   memset(mixBuffer, 0, hwBlockSize * 2 * sizeof(s32));

	for (u32 i = 0; i < PSP_AUDIO_CHANNEL_MAX + 1; i++)
   {
      if (!chans[i].reserved)
         continue;

      __AudioWakeThreads(chans[i], 0, hwBlockSize);

      if (!chans[i].sampleQueue.size())
         continue;

      const s16 *buf1 = 0, *buf2 = 0;
      size_t sz1, sz2;

      chans[i].sampleQueue.popPointers(hwBlockSize * 2, &buf1, &sz1, &buf2, &sz2);

      // Surprisingly hard to SIMD efficiently on SSE2 due to lack of 16-to-32-bit sign extension. NEON should be straight-forward though, and SSE4.1 can do it nicely.
      for (size_t s = 0; s < sz1; s++)
         mixBuffer[s] += buf1[s];
      if (buf2)
      {
         for (size_t s = 0; s < sz2; s++)
            mixBuffer[s + sz1] += buf2[s];
      }
   }

   if (queue_room() >= hwBlockSize * 2)
   {
      s16 *buf1 = 0, *buf2 = 0;
      size_t sz1, sz2;
      queue_pushPointers(hwBlockSize * 2, &buf1, &sz1, &buf2, &sz2);
      ClampBufferToS16(buf1, mixBuffer, sz1);
      if (buf2)
         ClampBufferToS16(buf2, mixBuffer + sz1, sz2);
   }
}

// numFrames is number of stereo frames.
// This is called from *outside* the emulator thread.
int __AudioMix(short *outstereo, int numFrames)
{
	// TODO: if mixFrequency != the actual output frequency, resample!
	int underrun = -1;
	s16 sampleL = 0;
	s16 sampleR = 0;

	const s16 *buf1 = 0, *buf2 = 0;
	size_t sz1, sz2;
	{
      queue_popPointers(numFrames * 2, &buf1, &sz1, &buf2, &sz2);

		memcpy(outstereo, buf1, sz1 * sizeof(s16));
		if (buf2)
			memcpy(outstereo + sz1, buf2, sz2 * sizeof(s16));
	}

	int remains = (int)(numFrames * 2 - sz1 - sz2);
	if (remains > 0)
		memset(outstereo + numFrames * 2 - remains, 0, remains*sizeof(s16));

	if (sz1 + sz2 < (size_t)numFrames)
		underrun = (int)(sz1 + sz2) / 2;
	return underrun >= 0 ? underrun : numFrames;
}
