#include "audio.h"

AudioPlayer::AudioPlayer()
{
  _pin = 255;
  _ready = false;
  _active = false;
  _inGap = false;
  _toneOn = false;
  _stepCount = 0;
  _currentStep = 0;
  _nextChangeMs = 0;
}

void AudioPlayer::begin(uint8_t buzzerPin)
{
  _pin = buzzerPin;
  _ready = true;

  pinMode(_pin, OUTPUT);
  digitalWrite(_pin, LOW);
  noTone(_pin);
}

void AudioPlayer::stop()
{
  if (_ready)
  {
    noTone(_pin);
    digitalWrite(_pin, LOW);
  }

  _active = false;
  _inGap = false;
  _toneOn = false;
  _stepCount = 0;
  _currentStep = 0;
  _nextChangeMs = 0;
}

bool AudioPlayer::playRequest(uint8_t requestCode)
{
  switch (requestCode)
  {
    case AUDIO_REQ_BUTTON:
      playButton();
      return true;

    case AUDIO_REQ_VICTORY:
      playVictory();
      return true;

    case AUDIO_REQ_MORSE_DOT:
      playMorseDot();
      return true;

    case AUDIO_REQ_MORSE_DASH:
      playMorseDash();
      return true;

    case AUDIO_REQ_SOLENOID_OPEN:
      playSolenoidOpen();
      return true;

    default:
      return false;
  }
}

void AudioPlayer::playButton()
{
  const AudioStep seq[] = {
    {2200, 35, 0}
  };

  startSequence(seq, sizeof(seq) / sizeof(seq[0]));
}

void AudioPlayer::playVictory()
{
  const AudioStep seq[] = {
    {988,  110, 35},
    {1319, 110, 35},
    {1568, 130, 45},
    {1976, 180, 60},
    {1568, 100, 35},
    {1976, 260, 0}
  };

  startSequence(seq, sizeof(seq) / sizeof(seq[0]));
}

void AudioPlayer::playMorseDot()
{
  const AudioStep seq[] = {
    {900, 120, 0}
  };

  startSequence(seq, sizeof(seq) / sizeof(seq[0]));
}

void AudioPlayer::playMorseDash()
{
  const AudioStep seq[] = {
    {900, 360, 0}
  };

  startSequence(seq, sizeof(seq) / sizeof(seq[0]));
}

void AudioPlayer::playSolenoidOpen()
{
  const AudioStep seq[] = {
    {1250, 120, 90},
    {1250, 120, 90},
    {1250, 120, 90},
    {1250, 120, 90},
    {1250, 120, 90},
    {1250, 120, 90},
    {1250, 120, 90},
    {1250, 120, 0}
  };

  startSequence(seq, sizeof(seq) / sizeof(seq[0]));
}

void AudioPlayer::startSequence(const AudioStep* steps, uint8_t count)
{
  if (!_ready || count == 0)
    return;

  if (count > MAX_STEPS)
    count = MAX_STEPS;

  stop();

  for (uint8_t i = 0; i < count; i++)
  {
    _steps[i] = steps[i];
  }

  _stepCount = count;
  _currentStep = 0;
  _active = true;
  _inGap = false;

  startCurrentStep();
}

void AudioPlayer::startCurrentStep()
{
  if (!_active || _currentStep >= _stepCount)
  {
    stop();
    return;
  }

  AudioStep step = _steps[_currentStep];

  if (step.frequency == 0 || step.durationMs == 0)
  {
    _toneOn = false;
    _inGap = true;
    _nextChangeMs = millis() + step.gapMs;
    return;
  }

  tone(_pin, step.frequency);
  _toneOn = true;
  _inGap = false;
  _nextChangeMs = millis() + step.durationMs;
}

void AudioPlayer::finishCurrentTone()
{
  noTone(_pin);
  digitalWrite(_pin, LOW);
  _toneOn = false;

  AudioStep step = _steps[_currentStep];

  if (step.gapMs > 0)
  {
    _inGap = true;
    _nextChangeMs = millis() + step.gapMs;
  }
  else
  {
    goToNextStep();
  }
}

void AudioPlayer::goToNextStep()
{
  _currentStep++;

  if (_currentStep >= _stepCount)
  {
    stop();
    return;
  }

  _inGap = false;
  startCurrentStep();
}

void AudioPlayer::update()
{
  if (!_ready || !_active)
    return;

  unsigned long now = millis();

  if (now < _nextChangeMs)
    return;

  if (_toneOn)
  {
    finishCurrentTone();
    return;
  }

  if (_inGap)
  {
    goToNextStep();
    return;
  }
}
