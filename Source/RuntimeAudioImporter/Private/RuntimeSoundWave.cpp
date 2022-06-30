// Fill out your copyright notice in the Description page of Project Settings.


#include "RuntimeSoundWave.h"
#include "DSP/EnvelopeFollower.h"
#include "DSP/SpectrumAnalyzer.h"

void URuntimeSoundWave::RuntimeBakeFFTAnalysis()
{
	// Clear any existing spectral data regardless of if it's enabled. If this was enabled and is now toggled, this will clear previous data.
	CookedSpectralTimeData.Reset();

	// Perform analysis if enabled on the sound wave
	
	{
		// If there are no frequencies to analyze, we can't do the analysis
		if (!RuntimeFrequenciesToAnalyze.Num())
		{
			UE_LOG(LogAudio, Warning, TEXT("Soundwave '%s' had baked FFT analysis enabled without specifying any frequencies to analyze."), *GetFullName());
			return;
		}

		if (SampleRate == 0 || NumChannels == 0)
		{
			UE_LOG(LogAudio, Error, TEXT("Failed to parse the raw imported data for '%s' for baked FFT analysis."), *GetFullName());
			return;
		}

		const uint32 NumFrames = (RawPCMDataSize / sizeof(int16)) / NumChannels;
		int16* InputData = (int16*)RawPCMData;

		Audio::FSpectrumAnalyzerSettings SpectrumAnalyzerSettings;
		switch (RuntimeFFTSize)
		{
		case ESoundWaveFFTSize::VerySmall_64:
			SpectrumAnalyzerSettings.FFTSize = Audio::FSpectrumAnalyzerSettings::EFFTSize::Min_64;
			break;

		case ESoundWaveFFTSize::Small_256:
			SpectrumAnalyzerSettings.FFTSize = Audio::FSpectrumAnalyzerSettings::EFFTSize::Small_256;
			break;

		default:
		case ESoundWaveFFTSize::Medium_512:
			SpectrumAnalyzerSettings.FFTSize = Audio::FSpectrumAnalyzerSettings::EFFTSize::Medium_512;
			break;

		case ESoundWaveFFTSize::Large_1024:
			SpectrumAnalyzerSettings.FFTSize = Audio::FSpectrumAnalyzerSettings::EFFTSize::Large_1024;
			break;

		case ESoundWaveFFTSize::VeryLarge_2048:
			SpectrumAnalyzerSettings.FFTSize = Audio::FSpectrumAnalyzerSettings::EFFTSize::VeryLarge_2048;
			break;

		}

		// Prepare the spectral envelope followers
		Audio::FInlineEnvelopeFollowerInitParams EnvelopeFollowerInitParams;
		EnvelopeFollowerInitParams.SampleRate = static_cast<float>(SampleRate) / static_cast<float>(FMath::Max(1, RuntimeFFTAnalysisFrameSize));
		EnvelopeFollowerInitParams.AttackTimeMsec = static_cast<float>(RuntimeFFTAnalysisAttackTime);
		EnvelopeFollowerInitParams.ReleaseTimeMsec = static_cast<float>(RuntimeFFTAnalysisReleaseTime);

		TArray<Audio::FInlineEnvelopeFollower> SpectralEnvelopeFollowers;
		for (int32 i = 0; i < RuntimeFrequenciesToAnalyze.Num(); i++)
		{
			SpectralEnvelopeFollowers.Emplace(EnvelopeFollowerInitParams);
		}

		// Build a new spectrum analyzer
		Audio::FSpectrumAnalyzer SpectrumAnalyzer(SpectrumAnalyzerSettings, (float)SampleRate);

		// The audio data block to use to submit audio data to the spectrum analyzer
		Audio::FAlignedFloatBuffer AnalysisData;
		check(FFTAnalysisFrameSize > 256);
		AnalysisData.Reserve(RuntimeFFTAnalysisFrameSize);

		float MaximumMagnitude = 0.0f;
		for (uint32 FrameIndex = 0; FrameIndex < NumFrames; ++FrameIndex)
		{
			// Get the averaged sample value of all the channels
			float SampleValue = 0.0f;
			for (uint16 ChannelIndex = 0; ChannelIndex < NumChannels; ++ChannelIndex)
			{
				SampleValue += (float)InputData[FrameIndex * NumChannels] / 32767.0f;
			}
			SampleValue /= NumChannels;

			// Accumate the samples in the scratch buffer
			AnalysisData.Add(SampleValue);

			// Until we reached the frame size
			if (AnalysisData.Num() == RuntimeFFTAnalysisFrameSize)
			{
				SpectrumAnalyzer.PushAudio(AnalysisData.GetData(), AnalysisData.Num());

				// Block while the analyzer does the analysis
				SpectrumAnalyzer.PerformAnalysisIfPossible(true);

				FSoundWaveSpectralTimeData NewData;

				// Don't need to lock here since we're doing this sync, but it's here as that's the expected pattern for the Spectrum analyzer
				SpectrumAnalyzer.LockOutputBuffer();

				// Get the magntiudes for the specified frequencies
				for (int32 Index = 0; Index < RuntimeFrequenciesToAnalyze.Num(); ++Index)
				{
					float Frequency = RuntimeFrequenciesToAnalyze[Index];
					FSoundWaveSpectralDataEntry DataEntry;
					DataEntry.Magnitude = SpectrumAnalyzer.GetMagnitudeForFrequency(Frequency);

					// Feed the magnitude through the spectral envelope follower for this band
					DataEntry.Magnitude = SpectralEnvelopeFollowers[Index].ProcessSample(DataEntry.Magnitude);

					// Track the max magnitude so we can later set normalized magnitudes
					if (DataEntry.Magnitude > MaximumMagnitude)
					{
						MaximumMagnitude = DataEntry.Magnitude;
					}

					NewData.Data.Add(DataEntry);
				}

				SpectrumAnalyzer.UnlockOutputBuffer();

				// The time stamp is derived from the frame index and sample rate
				NewData.TimeSec = FMath::Max((float)(FrameIndex - RuntimeFFTAnalysisFrameSize + 1) / SampleRate, 0.0f);

				CookedSpectralTimeData.Add(NewData);

				AnalysisData.Reset();
			}
		}

		// Sort predicate for sorting spectral data by time (lowest first)
		struct FSortSpectralDataByTime
		{
			FORCEINLINE bool operator()(const FSoundWaveSpectralTimeData& A, const FSoundWaveSpectralTimeData& B) const
			{
				return A.TimeSec < B.TimeSec;
			}
		};

		CookedSpectralTimeData.Sort(FSortSpectralDataByTime());

		// It's possible for the maximum magnitude to be 0.0 if the audio file was silent.
		if (MaximumMagnitude > 0.0f)
		{
			// Normalize all the magnitude values based on the highest magnitude
			for (FSoundWaveSpectralTimeData& SpectralTimeData : CookedSpectralTimeData)
			{
				for (FSoundWaveSpectralDataEntry& DataEntry : SpectralTimeData.Data)
				{
					DataEntry.NormalizedMagnitude = DataEntry.Magnitude / MaximumMagnitude;
				}
			}
		}

	}
}

void URuntimeSoundWave::RuntimeBakeEnvelopeAnalysis()
{
	// Clear any existing envelope data regardless of if it's enabled. If this was enabled and is now toggled, this will clear previous data.
	CookedEnvelopeTimeData.Reset();

	// Perform analysis if enabled on the sound wave
	{

		const uint32 NumFrames = (RawPCMDataSize / sizeof(int16)) / NumChannels;
		int16* InputData = (int16*)RawPCMData;

		Audio::FInlineEnvelopeFollowerInitParams EnvelopeFollowerInitParams;
		EnvelopeFollowerInitParams.SampleRate = SampleRate;
		EnvelopeFollowerInitParams.AttackTimeMsec = static_cast<float>(RuntimeEnvelopeFollowerAttackTime);
		EnvelopeFollowerInitParams.ReleaseTimeMsec = static_cast<float>(RuntimeEnvelopeFollowerReleaseTime);
		Audio::FInlineEnvelopeFollower EnvelopeFollower(EnvelopeFollowerInitParams);

		for (uint32 FrameIndex = 0; FrameIndex < NumFrames; ++FrameIndex)
		{
			// Get the averaged sample value of all the channels
			float SampleValue = 0.0f;
			for (uint16 ChannelIndex = 0; ChannelIndex < NumChannels; ++ChannelIndex)
			{
				SampleValue += (float)InputData[FrameIndex * NumChannels] / 32767.0f;
			}
			SampleValue /= NumChannels;

			float Output = EnvelopeFollower.ProcessSample(SampleValue);
			Output = FMath::Clamp(Output, 0.f, 1.f);



			// Until we reached the frame size
			if (FrameIndex % RuntimeEnvelopeFollowerFrameSize == 0)
			{
				FSoundWaveEnvelopeTimeData NewData;
				NewData.Amplitude = Output;
				NewData.TimeSec = (float)FrameIndex / SampleRate;
				CookedEnvelopeTimeData.Add(NewData);
			}
		}
	}
}