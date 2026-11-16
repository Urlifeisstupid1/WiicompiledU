// SPDX-License-Identifier: GPL-3.0-or-later
//
// Wii U AudioBackend (replaces src/audio_backend.cpp, which uses SDL3 audio).
//
// Phase 2 (headless bring-up): a null sink. It accepts and drops samples so the
// AI/AX HLE runs at its normal pace. Phase 4 replaces this with an sndcore2 (AX)
// voice on the Wii U. See PORTING_NOTES.md, D10.
#include "audio_backend.h"

AudioBackend& AudioBackend::Instance()
{
    static AudioBackend instance;
    return instance;
}

bool AudioBackend::Init(uint32_t sampleRate, uint32_t channels)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sampleRate = sampleRate;
    m_channels = channels;
    m_initialized = true;
    return true;
}

void AudioBackend::Shutdown()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_initialized = false;
}

bool AudioBackend::PushWiiAiSamplesBE16(const uint8_t* data, size_t bytes)
{
    (void)data;
    (void)bytes;
    return true;
}

bool AudioBackend::PushSamplesLE16(const int16_t* samples, size_t sampleCount)
{
    (void)samples;
    (void)sampleCount;
    return true;
}

void AudioBackend::SetMasterVolume(float volume)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_masterVolume = volume;
}

void AudioBackend::SetMuted(bool muted)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_muted = muted;
}

void AudioBackend::SetSilenced(bool silenced)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_silenced = silenced;
}
