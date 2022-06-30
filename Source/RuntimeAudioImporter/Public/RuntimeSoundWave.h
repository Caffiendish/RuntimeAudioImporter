// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Sound/SoundWave.h"
#include "RuntimeSoundWave.generated.h"

/**
 *
 */
UCLASS()
class RUNTIMEAUDIOIMPORTER_API URuntimeSoundWave : public USoundWave
{
	GENERATED_BODY()

public:

	UFUNCTION(BlueprintCallable, Category = "Runtime Audio")
	void RuntimeBakeFFTAnalysis();

	UFUNCTION(BlueprintCallable, Category = "Runtime Audio")
	void RuntimeBakeEnvelopeAnalysis();


	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 RuntimeEnvelopeFollowerFrameSize = 1024;
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 RuntimeEnvelopeFollowerAttackTime = 10;
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 RuntimeEnvelopeFollowerReleaseTime = 100;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 RuntimeFFTAnalysisReleaseTime = 3000;
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 RuntimeFFTAnalysisAttackTime = 10;
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 RuntimeFFTAnalysisFrameSize = 1024;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<float> RuntimeFrequenciesToAnalyze = {100, 500, 1000, 5000};

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	ESoundWaveFFTSize RuntimeFFTSize = ESoundWaveFFTSize::Medium_512;
};
