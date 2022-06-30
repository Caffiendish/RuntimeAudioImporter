// Georgy Treshchev 2022.

#include "RuntimeAudioImporterLibrary.h"

#include "RuntimeAudioImporterDefines.h"
#include "RuntimeAudioImporterTypes.h"

#include "Transcoders/MP3Transcoder.h"
#include "Transcoders/WAVTranscoder.h"
#include "Transcoders/FlacTranscoder.h"
#include "Transcoders/VorbisTranscoder.h"
#include "Transcoders/RAWTranscoder.h"

#include "Misc/FileHelper.h"
#include "Async/Async.h"

URuntimeAudioImporterLibrary* URuntimeAudioImporterLibrary::CreateRuntimeAudioImporter()
{
	return NewObject<URuntimeAudioImporterLibrary>();
}

bool LoadAudioFileToArray(TArray<uint8>& AudioData, const FString& FilePath)
{
	// Filling AudioBuffer with a binary file
	if (!FFileHelper::LoadFileToArray(AudioData, *FilePath))
	{
		return false;
	}

	// Removing unused two unitialized bytes
	AudioData.RemoveAt(AudioData.Num() - 2, 2);

	return true;
}

void URuntimeAudioImporterLibrary::ImportAudioFromFile(const FString& FilePath, EAudioFormat Format, const bool bFillPCMBuffer)
{
	// Checking if the file exists
	if (!FPaths::FileExists(FilePath))
	{
		OnResult_Internal(nullptr, ETranscodingStatus::AudioDoesNotExist);
		return;
	}

	// Getting the audio format
	Format = Format == EAudioFormat::Auto ? GetAudioFormat(FilePath) : Format;
	Format = Format == EAudioFormat::Invalid ? EAudioFormat::Auto : Format;

	TArray<uint8> AudioBuffer;

	// Filling AudioBuffer with a binary file
	if (!LoadAudioFileToArray(AudioBuffer, *FilePath))
	{
		OnResult_Internal(nullptr, ETranscodingStatus::LoadFileToArrayError);
		return;
	}

	ImportAudioFromBuffer(MoveTemp(AudioBuffer), Format, bFillPCMBuffer);
}

void URuntimeAudioImporterLibrary::ImportAudioFromRAWFile(const FString& FilePath, ERAWAudioFormat Format, const bool bFillPCMBuffer, int32 SampleRate, int32 NumOfChannels)
{
	if (!FPaths::FileExists(FilePath))
	{
		OnResult_Internal(nullptr, ETranscodingStatus::AudioDoesNotExist);
		return;
	}

	OnProgress_Internal(5);

	TArray<uint8> AudioBuffer;
	if (!LoadAudioFileToArray(AudioBuffer, *FilePath))
	{
		OnResult_Internal(nullptr, ETranscodingStatus::LoadFileToArrayError);
		return;
	}

	OnProgress_Internal(35);

	AsyncTask(ENamedThreads::AnyBackgroundHiPriTask, [this, AudioBuffer = MoveTemp(AudioBuffer), Format, SampleRate, NumOfChannels, bFillPCMBuffer]()
		{
			ImportAudioFromRAWBuffer(AudioBuffer, Format, bFillPCMBuffer, SampleRate, NumOfChannels);
		});
}

void URuntimeAudioImporterLibrary::ImportAudioFromRAWBuffer(TArray<uint8> RAWBuffer, ERAWAudioFormat Format, const bool bFillPCMBuffer, int32 SampleRate, int32 NumOfChannels)
{
	uint8* RAWData{ RAWBuffer.GetData() };
	const int32 RAWDataSize{ RAWBuffer.Num() };

	int16* PCMData{ nullptr };
	int32 PCMDataSize{ 0 };

	// Transcoding RAW data to 32-bit float data
	{
		switch (Format)
		{
		case ERAWAudioFormat::Int16:
		{
			PCMData = static_cast<int16*>(FMemory::Memcpy(FMemory::Malloc(PCMDataSize), RAWData, RAWDataSize));
			PCMDataSize = RAWDataSize;
			break;
		}
		case ERAWAudioFormat::Int32:
		{
			RAWTranscoder::TranscodeRAWData<int32, int16>(reinterpret_cast<int32*>(RAWData), RAWDataSize, PCMData, PCMDataSize);
			break;
		}
		case ERAWAudioFormat::UInt8:
		{
			RAWTranscoder::TranscodeRAWData<uint8, int16>(RAWData, RAWDataSize, PCMData, PCMDataSize);
			break;
		}
		case ERAWAudioFormat::Float32:
		{
			RAWTranscoder::TranscodeRAWData<float, int16>(reinterpret_cast<float*>(RAWData), RAWDataSize, PCMData, PCMDataSize);
			break;
		}
		}
	}

	if (!PCMData || PCMDataSize < 0)
	{
		OnResult_Internal(nullptr, ETranscodingStatus::FailedToReadAudioDataArray);
		return;
	}

	ImportAudioFromInt16Buffer(reinterpret_cast<uint8*>(PCMData), PCMDataSize, bFillPCMBuffer, SampleRate, NumOfChannels);
}

void URuntimeAudioImporterLibrary::ImportAudioFromBuffer(TArray<uint8> AudioData, EAudioFormat AudioFormat, const bool bFillPCMBuffer)
{
	if (AudioFormat == EAudioFormat::Wav && !WAVTranscoder::CheckAndFixWavDurationErrors(AudioData)) return;

	if (AudioFormat == EAudioFormat::Auto)
	{
		AudioFormat = GetAudioFormat(AudioData.GetData(), AudioData.Num());
	}

	AsyncTask(ENamedThreads::AnyBackgroundHiPriTask, [this, AudioData = MoveTemp(AudioData), AudioFormat, bFillPCMBuffer]()
		{
			OnProgress_Internal(5);

			if (AudioFormat == EAudioFormat::Invalid)
			{
				UE_LOG(LogRuntimeAudioImporter, Error, TEXT("Undefined audio data format for import"));
				OnResult_Internal(nullptr, ETranscodingStatus::InvalidAudioFormat);
				return;
			}

			uint8* EncodedAudioDataPtr = static_cast<uint8*>(FMemory::Memcpy(FMemory::Malloc(AudioData.Num()), AudioData.GetData(), AudioData.Num()));

			FEncodedAudioStruct EncodedAudioInfo(EncodedAudioDataPtr, AudioData.Num(), AudioFormat);

			OnProgress_Internal(10);

			FDecodedAudioStruct DecodedAudioInfo;
			if (!DecodeAudioData(EncodedAudioInfo, DecodedAudioInfo))
			{
				OnResult_Internal(nullptr, ETranscodingStatus::FailedToReadAudioDataArray);
				return;
			}

			OnProgress_Internal(65);

			AsyncTask(ENamedThreads::GameThread, [this, DecodedAudioInfo = MoveTemp(DecodedAudioInfo), bFillPCMBuffer]()
				{
					ImportAudioFromDecodedInfo(DecodedAudioInfo, bFillPCMBuffer);
				});
		});
}

void URuntimeAudioImporterLibrary::TranscodeRAWDataFromBuffer(TArray<uint8> RAWData_From, ERAWAudioFormat RAWFrom, TArray<uint8>& RAWData_To, ERAWAudioFormat RAWTo)
{
	TArray<uint8> IntermediateRAWBuffer;

	// Transcoding of all formats to unsigned 8-bit PCM format (intermediate)
	switch (RAWFrom)
	{
	case ERAWAudioFormat::Int16:
	{
		RAWTranscoder::TranscodeRAWData<int16, uint8>(RAWData_From, IntermediateRAWBuffer);
		break;
	}
	case ERAWAudioFormat::Int32:
	{
		RAWTranscoder::TranscodeRAWData<int32, uint8>(RAWData_From, IntermediateRAWBuffer);
		break;
	}
	case ERAWAudioFormat::UInt8:
	{
		IntermediateRAWBuffer = RAWData_From;
		break;
	}
	case ERAWAudioFormat::Float32:
	{
		RAWTranscoder::TranscodeRAWData<float, uint8>(RAWData_From, IntermediateRAWBuffer);
		break;
	}
	}

	RAWData_From.Empty();

	// Transcoding unsigned 8-bit PCM to the specified format
	switch (RAWTo)
	{
	case ERAWAudioFormat::Int16:
	{
		RAWTranscoder::TranscodeRAWData<uint8, int16>(IntermediateRAWBuffer, RAWData_To);
		break;
	}
	case ERAWAudioFormat::Int32:
	{
		RAWTranscoder::TranscodeRAWData<uint8, int32>(IntermediateRAWBuffer, RAWData_To);
		break;
	}
	case ERAWAudioFormat::UInt8:
	{
		RAWData_To = IntermediateRAWBuffer;
		break;
	}
	case ERAWAudioFormat::Float32:
	{
		RAWTranscoder::TranscodeRAWData<uint8, float>(IntermediateRAWBuffer, RAWData_To);
		break;
	}
	}
}

bool URuntimeAudioImporterLibrary::TranscodeRAWDataFromFile(const FString& FilePathFrom, ERAWAudioFormat FormatFrom, const FString& FilePathTo, ERAWAudioFormat FormatTo)
{
	TArray<uint8> RAWBufferFrom;

	// Loading a file into a byte array
	if (!LoadAudioFileToArray(RAWBufferFrom, *FilePathFrom))
	{
		UE_LOG(LogRuntimeAudioImporter, Error, TEXT("Something went wrong when reading RAW data on the path '%s'"), *FilePathFrom);
		return false;
	}

	TArray<uint8> RAWBufferTo;
	TranscodeRAWDataFromBuffer(RAWBufferFrom, FormatFrom, RAWBufferTo, FormatTo);

	// Writing a file to a specified location
	if (!FFileHelper::SaveArrayToFile(MoveTemp(RAWBufferTo), *FilePathTo))
	{
		UE_LOG(LogRuntimeAudioImporter, Error, TEXT("Something went wrong when saving RAW data to the path '%s'"), *FilePathTo);
		return false;
	}

	return true;
}

void URuntimeAudioImporterLibrary::ImportAudioFromInt16Buffer(uint8* PCMData, const int32 PCMDataSize, const bool bFillPCMBuffer, const int32 SampleRate, const int32 NumOfChannels)
{
	FDecodedAudioStruct DecodedAudioInfo;

	// Filling in the required information
	{
		DecodedAudioInfo.PCMInfo.PCMData = FBulkDataBuffer<uint8>(PCMData, PCMDataSize);
		DecodedAudioInfo.PCMInfo.PCMNumOfFrames = PCMDataSize / sizeof(int16) / NumOfChannels;

		DecodedAudioInfo.SoundWaveBasicInfo.NumOfChannels = NumOfChannels;
		DecodedAudioInfo.SoundWaveBasicInfo.SampleRate = SampleRate;
		DecodedAudioInfo.SoundWaveBasicInfo.Duration = static_cast<float>(DecodedAudioInfo.PCMInfo.PCMNumOfFrames) / SampleRate;
	}

	OnProgress_Internal(50);

	// Finalizing import
	ImportAudioFromDecodedInfo(DecodedAudioInfo, bFillPCMBuffer);
}

void URuntimeAudioImporterLibrary::ImportAudioFromDecodedInfo(const FDecodedAudioStruct& DecodedAudioInfo, const bool bFillPCMBuffer)
{
	URuntimeSoundWave* SoundWaveRef = CreateImportedSoundWave();

	if (SoundWaveRef == nullptr)
	{
		UE_LOG(LogRuntimeAudioImporter, Error, TEXT("Something went wrong while creating the imported sound wave"));
		OnResult_Internal(nullptr, ETranscodingStatus::SoundWaveDeclarationError);
		return;
	}
	OnProgress_Internal(70);
	// Filling in a sound wave basic information (e.g. duration, number of channels, etc)


	FAudioThread::RunCommandOnAudioThread([this, SoundWaveRef, DecodedAudioInfo, bFillPCMBuffer]()
		{
			SoundWaveRef->Duration = DecodedAudioInfo.SoundWaveBasicInfo.Duration;
			SoundWaveRef->SetSampleRate(DecodedAudioInfo.SoundWaveBasicInfo.SampleRate);
			SoundWaveRef->NumChannels = DecodedAudioInfo.SoundWaveBasicInfo.NumOfChannels;
			SoundWaveRef->SoundGroup = SOUNDGROUP_Default;

			if (SoundWaveRef->NumChannels == 4)
			{
				SoundWaveRef->bIsAmbisonics = 1;
			}

			SoundWaveRef->bProcedural = false;

			OnProgress_Internal(75);

			
			if (bFillPCMBuffer)
			{
				FillPCMBuffer(SoundWaveRef, DecodedAudioInfo);
			}
			else
			{
				if(!FillRawWaveBuffer(SoundWaveRef, DecodedAudioInfo))
				{
					UE_LOG(LogRuntimeAudioImporter, Error, TEXT("Unable to encode PCM to WAV format due to transcoder error"));
					OnResult_Internal(nullptr, ETranscodingStatus::InvalidAudioFormat);
				}
			}

			OnProgress_Internal(95);

			UE_LOG(LogRuntimeAudioImporter, Log, TEXT("The audio data was successfully imported. Information about imported data:\n%s"), *DecodedAudioInfo.ToString());
			OnProgress_Internal(100);
			OnResult_Internal(SoundWaveRef, ETranscodingStatus::SuccessfulImport);
		});
}


bool URuntimeAudioImporterLibrary::FillRawWaveBuffer(URuntimeSoundWave* SoundWaveRef, const FDecodedAudioStruct& DecodedAudioInfo)
{
	SoundWaveRef->DecompressionType = EDecompressionType::DTYPE_Streaming;

	// Encoding to WAV format
	FEncodedAudioStruct EncodedAudioInfo;
	if (!WAVTranscoder::Encode(DecodedAudioInfo, EncodedAudioInfo, FWAVEncodingFormat(EWAVEncodingFormat::FORMAT_PCM, 16)))
	{
		return false;
	}

	SoundWaveRef->RawData.Lock(LOCK_READ_WRITE);
	FMemory::Memcpy(SoundWaveRef->RawData.Realloc(EncodedAudioInfo.AudioData.GetView().Num()), EncodedAudioInfo.AudioData.GetView().GetData(), EncodedAudioInfo.AudioData.GetView().Num());
	SoundWaveRef->RawData.Unlock();

	SoundWaveRef->SetPrecacheState(ESoundWavePrecacheState::Done);

	UE_LOG(LogRuntimeAudioImporter, Log, TEXT("Filled RAW Wave Buffer with size '%d'"), EncodedAudioInfo.AudioData.GetView().Num());
	return true;
}

void URuntimeAudioImporterLibrary::FillPCMBuffer(URuntimeSoundWave* SoundWaveRef, const FDecodedAudioStruct& DecodedAudioInfo)
{
		SoundWaveRef->DecompressionType = EDecompressionType::DTYPE_Native;

		SoundWaveRef->RawPCMDataSize = DecodedAudioInfo.PCMInfo.PCMData.GetView().Num();
		SoundWaveRef->RawPCMData = static_cast<uint8*>(FMemory::Malloc(SoundWaveRef->RawPCMDataSize));
		FMemory::Memcpy(SoundWaveRef->RawPCMData, DecodedAudioInfo.PCMInfo.PCMData.GetView().GetData(), SoundWaveRef->RawPCMDataSize);


		SoundWaveRef->SetPrecacheState(ESoundWavePrecacheState::Done);

		UE_LOG(LogRuntimeAudioImporter, Log, TEXT("Filled PCM Buffer with size '%d'"), SoundWaveRef->RawPCMDataSize);
}


EAudioFormat URuntimeAudioImporterLibrary::GetAudioFormat(const FString& FilePath)
{
	const FString& Extension{ FPaths::GetExtension(FilePath, false).ToLower() };

	if (Extension == TEXT("mp3"))
	{
		return EAudioFormat::Mp3;
	}

	if (Extension == TEXT("wav") || Extension == TEXT("wave"))
	{
		return EAudioFormat::Wav;
	}

	if (Extension == TEXT("flac"))
	{
		return EAudioFormat::Flac;
	}

	if (Extension == TEXT("ogg") || Extension == TEXT("oga") || Extension == TEXT("sb0"))
	{
		return EAudioFormat::OggVorbis;
	}

	UE_LOG(LogRuntimeAudioImporter, Warning, TEXT("Unable to determine audio file format with path '%s' by name"), *FilePath);

	return EAudioFormat::Invalid;
}

EAudioFormat URuntimeAudioImporterLibrary::GetAudioFormatAdvanced(const TArray<uint8>& AudioData)
{
	return GetAudioFormat(AudioData.GetData(), AudioData.Num());
}

EAudioFormat URuntimeAudioImporterLibrary::GetAudioFormat(const uint8* AudioData, int32 AudioDataSize)
{
	if (MP3Transcoder::CheckAudioFormat(AudioData, AudioDataSize))
	{
		return EAudioFormat::Mp3;
	}

	if (WAVTranscoder::CheckAudioFormat(AudioData, AudioDataSize))
	{
		return EAudioFormat::Wav;
	}

	if (FlacTranscoder::CheckAudioFormat(AudioData, AudioDataSize))
	{
		return EAudioFormat::Flac;
	}

	if (VorbisTranscoder::CheckAudioFormat(AudioData, AudioDataSize))
	{
		return EAudioFormat::OggVorbis;
	}

	UE_LOG(LogRuntimeAudioImporter, Error, TEXT("Unable to determine audio data format"));

	return EAudioFormat::Invalid;
}

FString URuntimeAudioImporterLibrary::ConvertSecondsToString(int32 Seconds)
{
	FString FinalString;

	const int32 NewHours = Seconds / 3600;
	if (NewHours > 0)
	{
		FinalString += ((NewHours < 10) ? TEXT("0") + FString::FromInt(NewHours) : FString::FromInt(NewHours)) + TEXT(":");
	}

	Seconds = Seconds % 3600;

	const int32 NewMinutes = Seconds / 60;
	FinalString += ((NewMinutes < 10) ? TEXT("0") + FString::FromInt(NewMinutes) : FString::FromInt(NewMinutes)) + TEXT(":");

	const int32 NewSeconds = Seconds % 60;
	FinalString += (NewSeconds < 10) ? TEXT("0") + FString::FromInt(NewSeconds) : FString::FromInt(NewSeconds);

	return FinalString;
}

bool URuntimeAudioImporterLibrary::DecodeAudioData(FEncodedAudioStruct& EncodedAudioInfo, FDecodedAudioStruct& DecodedAudioInfo)
{
	if (EncodedAudioInfo.AudioFormat == EAudioFormat::Auto)
	{
		EncodedAudioInfo.AudioFormat = GetAudioFormat(EncodedAudioInfo.AudioData.GetView().GetData(), EncodedAudioInfo.AudioData.GetView().Num());
	}

	switch (EncodedAudioInfo.AudioFormat)
	{
	case EAudioFormat::Mp3:
	{
		if (!MP3Transcoder::Decode(EncodedAudioInfo, DecodedAudioInfo))
		{
			UE_LOG(LogRuntimeAudioImporter, Error, TEXT("Something went wrong while decoding Mp3 audio data"));
			return false;
		}
		break;
	}
	case EAudioFormat::Wav:
	{
		if (!WAVTranscoder::Decode(EncodedAudioInfo, DecodedAudioInfo))
		{
			UE_LOG(LogRuntimeAudioImporter, Error, TEXT("Something went wrong while decoding Wav audio data"));
			return false;
		}
		break;
	}
	case EAudioFormat::Flac:
	{
		if (!FlacTranscoder::Decode(EncodedAudioInfo, DecodedAudioInfo))
		{
			UE_LOG(LogRuntimeAudioImporter, Error, TEXT("Something went wrong while decoding Flac audio data"));
			return false;
		}
		break;
	}
	case EAudioFormat::OggVorbis:
	{
		if (!VorbisTranscoder::Decode(EncodedAudioInfo, DecodedAudioInfo))
		{
			UE_LOG(LogRuntimeAudioImporter, Error, TEXT("Something went wrong while decoding Vorbis audio data"));
			return false;
		}
		break;
	}
	default:
	{
		UE_LOG(LogRuntimeAudioImporter, Error, TEXT("Undefined audio data format for decoding"));
		return false;
	}
	}

	return true;
}

bool URuntimeAudioImporterLibrary::EncodeAudioData(const FDecodedAudioStruct& DecodedAudioInfo, FEncodedAudioStruct& EncodedAudioInfo, uint8 Quality)
{
	if (EncodedAudioInfo.AudioFormat == EAudioFormat::Auto || EncodedAudioInfo.AudioFormat == EAudioFormat::Invalid)
	{
		UE_LOG(LogRuntimeAudioImporter, Error, TEXT("Undefined audio data format for encoding"));
		return false;
	}

	switch (EncodedAudioInfo.AudioFormat)
	{
	case EAudioFormat::Mp3:
	{
		UE_LOG(LogRuntimeAudioImporter, Error, TEXT("MP3 format is not currently supported for encoding"));
		break;
	}
	case EAudioFormat::Wav:
	{
		if (!WAVTranscoder::Encode(DecodedAudioInfo, EncodedAudioInfo, FWAVEncodingFormat(EWAVEncodingFormat::FORMAT_IEEE_FLOAT, 32)))
		{
			UE_LOG(LogRuntimeAudioImporter, Error, TEXT("Something went wrong while encoding Wav audio data"));
			return false;
		}
		break;
	}
	case EAudioFormat::Flac:
	{
		UE_LOG(LogRuntimeAudioImporter, Error, TEXT("Flac format is not currently supported for encoding"));
		break;
	}
	case EAudioFormat::OggVorbis:
	{
		if (!VorbisTranscoder::Encode(DecodedAudioInfo, EncodedAudioInfo, Quality))
		{
			UE_LOG(LogRuntimeAudioImporter, Error, TEXT("Something went wrong while encoding Vorbis audio data"));
			return false;
		}
		break;
	}
	default:
	{
		UE_LOG(LogRuntimeAudioImporter, Error, TEXT("Undefined audio data format for encoding"));
		return false;
	}
	}

	return true;
}

URuntimeSoundWave* URuntimeAudioImporterLibrary::CreateImportedSoundWave() const
{
	return NewObject<URuntimeSoundWave>();
}

void URuntimeAudioImporterLibrary::OnProgress_Internal(int32 Percentage)
{
	AsyncTask(ENamedThreads::GameThread, [this, Percentage]()
		{
			if (OnProgress.IsBound())
			{
				OnProgress.Broadcast(Percentage);
			}

			if (OnProgressNative.IsBound())
			{
				OnProgressNative.Broadcast(Percentage);
			}
		});
}

void URuntimeAudioImporterLibrary::OnResult_Internal(URuntimeSoundWave* SoundWaveRef, ETranscodingStatus Status)
{
	AsyncTask(ENamedThreads::GameThread, [this, SoundWaveRef, Status]()
		{
			bool bBroadcasted{ false };

			if (OnResultNative.IsBound())
			{
				bBroadcasted = true;
				OnResultNative.Broadcast(this, SoundWaveRef, Status);
			}

			if (OnResult.IsBound())
			{
				bBroadcasted = true;
				OnResult.Broadcast(this, SoundWaveRef, Status);
			}

			if (!bBroadcasted)
			{
				UE_LOG(LogRuntimeAudioImporter, Error, TEXT("You did not bind to the delegate to get the result of the import"));
			}
		});
}
