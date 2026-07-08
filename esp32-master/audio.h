#ifndef AUDIO_H
#define AUDIO_H

#include <Arduino.h>

// I2C audio request codes.
// De puzzelmodules sturen deze waarden terug naar de main-module.
static const uint8_t AUDIO_REQ_BUTTON        = 10;
static const uint8_t AUDIO_REQ_VICTORY       = 11;
static const uint8_t AUDIO_REQ_MORSE_DOT     = 12;
static const uint8_t AUDIO_REQ_MORSE_DASH    = 13;
static const uint8_t AUDIO_REQ_SOLENOID_OPEN = 14;

struct AudioStep
{
  uint16_t frequency;
  uint16_t durationMs;
  uint16_t gapMs;
};

class AudioPlayer
{
public:
  AudioPlayer();

  void begin(uint8_t buzzerPin);
  void update();
  void stop();

  bool playRequest(uint8_t requestCode);

  void playButton();
  void playVictory();
  void playMorseDot();
  void playMorseDash();
  void playSolenoidOpen();

private:
  static const uint8_t MAX_STEPS = 20;

  uint8_t _pin;
  bool _ready;
  bool _active;
  bool _inGap;
  bool _toneOn;

  AudioStep _steps[MAX_STEPS];
  uint8_t _stepCount;
  uint8_t _currentStep;
  unsigned long _nextChangeMs;

  void startSequence(const AudioStep* steps, uint8_t count);
  void startCurrentStep();
  void finishCurrentTone();
  void goToNextStep();
};

#endif
