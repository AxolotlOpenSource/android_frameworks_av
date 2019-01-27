/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "SoftOpus"

#include <cstring>
#include <stdint.h>

#include <log/log.h>

#include "OpusHeader.h"

namespace android {

// Opus uses Vorbis channel mapping, and Vorbis channel mapping specifies
// mappings for up to 8 channels. This information is part of the Vorbis I
// Specification:
// http://www.xiph.org/vorbis/doc/Vorbis_I_spec.html
constexpr int kMaxChannels = 8;

constexpr uint8_t kOpusChannelMap[kMaxChannels][kMaxChannels] = {
        {0},
        {0, 1},
        {0, 2, 1},
        {0, 1, 2, 3},
        {0, 4, 1, 2, 3},
        {0, 4, 1, 2, 3, 5},
        {0, 4, 1, 2, 3, 5, 6},
        {0, 6, 1, 2, 3, 4, 5, 7},
};

// Opus always has a 48kHz output rate. This is true for all Opus, not just this
// implementation.
constexpr int kRate = 48000;
// Size of the Opus header excluding optional mapping information.
constexpr size_t kOpusHeaderSize = 19;
// Offset to magic string that starts Opus header.
constexpr size_t kOpusHeaderLabelOffset = 0;
// Offset to Opus version in the Opus header.
constexpr size_t kOpusHeaderVersionOffset = 8;
// Offset to the channel count byte in the Opus header.
constexpr size_t kOpusHeaderChannelsOffset = 9;
// Offset to the pre-skip value in the Opus header.
constexpr size_t kOpusHeaderSkipSamplesOffset = 10;
// Offset to sample rate in the Opus header.
constexpr size_t kOpusHeaderSampleRateOffset = 12;
// Offset to the gain value in the Opus header.
constexpr size_t kOpusHeaderGainOffset = 16;
// Offset to the channel mapping byte in the Opus header.
constexpr size_t kOpusHeaderChannelMappingOffset = 18;
// Opus Header contains a stream map. The mapping values are in the header
// beyond the always present |kOpusHeaderSize| bytes of data. The mapping
// data contains stream count, coupling information, and per channel mapping
// values:
//   - Byte 0: Number of streams.
//   - Byte 1: Number coupled.
//   - Byte 2: Starting at byte 2 are |header->channels| uint8 mapping
//             values.
// Offset to the number of streams in the Opus header.
constexpr size_t kOpusHeaderNumStreamsOffset = 19;
// Offset to the number of streams that are coupled in the Opus header.
constexpr size_t kOpusHeaderNumCoupledStreamsOffset = 20;
// Offset to the stream to channel mapping in the Opus header.
constexpr size_t kOpusHeaderStreamMapOffset = 21;
// Maximum packet size used in Xiph's opusdec.
constexpr int kMaxOpusOutputPacketSizeSamples = 960 * 6;

// Default audio output channel layout. Used to initialize |stream_map| in
// OpusHeader, and passed to opus_multistream_decoder_create() when the header
// does not contain mapping information. The values are valid only for mono and
// stereo output: Opus streams with more than 2 channels require a stream map.
constexpr int kMaxChannelsWithDefaultLayout = 2;
constexpr uint8_t kDefaultOpusChannelLayout[kMaxChannelsWithDefaultLayout] = {0, 1};

static uint16_t ReadLE16(const uint8_t* data, size_t data_size, uint32_t read_offset) {
    // check whether the 2nd byte is within the buffer
    if (read_offset + 1 >= data_size) return 0;
    uint16_t val;
    val = data[read_offset];
    val |= data[read_offset + 1] << 8;
    return val;
}

// Parses Opus Header. Header spec: http://wiki.xiph.org/OggOpus#ID_Header
bool ParseOpusHeader(const uint8_t* data, size_t data_size, OpusHeader* header) {
    if (data_size < kOpusHeaderSize) {
        ALOGV("Header size is too small.");
        return false;
    }
    header->channels = data[kOpusHeaderChannelsOffset];

    if (header->channels < 1 || header->channels > kMaxChannels) {
        ALOGV("Invalid Header, bad channel count: %d", header->channels);
        return false;
    }
    header->skip_samples = ReadLE16(data, data_size, kOpusHeaderSkipSamplesOffset);
    header->gain_db = static_cast<int16_t>(ReadLE16(data, data_size, kOpusHeaderGainOffset));
    header->channel_mapping = data[kOpusHeaderChannelMappingOffset];
    if (!header->channel_mapping) {
        if (header->channels > kMaxChannelsWithDefaultLayout) {
            ALOGV("Invalid Header, missing stream map.");
            return false;
        }
        header->num_streams = 1;
        header->num_coupled = header->channels > 1;
        header->stream_map[0] = 0;
        header->stream_map[1] = 1;
        return true;
    }
    if (data_size < kOpusHeaderStreamMapOffset + header->channels) {
        ALOGV("Invalid stream map; insufficient data for current channel "
              "count: %d",
              header->channels);
        return false;
    }
    header->num_streams = data[kOpusHeaderNumStreamsOffset];
    header->num_coupled = data[kOpusHeaderNumCoupledStreamsOffset];
    if (header->num_streams + header->num_coupled != header->channels) {
        ALOGV("Inconsistent channel mapping.");
        return false;
    }
    for (int i = 0; i < header->channels; ++i)
        header->stream_map[i] = data[kOpusHeaderStreamMapOffset + i];
    return true;
}

int WriteOpusHeader(const OpusHeader &header, int input_sample_rate,
                    uint8_t* output, size_t output_size) {
    // See https://wiki.xiph.org/OggOpus#ID_Header.
    const size_t total_size = kOpusHeaderStreamMapOffset + header.channels;
    if (output_size < total_size) {
        ALOGE("Output buffer too small for header.");
        return -1;
    }

    // ensure entire header is cleared, even though we overwrite much of it below
    memset(output, 0, output_size);

    // Set magic signature.
    memcpy(output + kOpusHeaderLabelOffset, "OpusHead", 8);
    // Set Opus version.
    output[kOpusHeaderVersionOffset] = 1;
    // Set channel count.
    output[kOpusHeaderChannelsOffset] = (uint8_t)header.channels;
    // Set pre-skip
    memcpy(output + kOpusHeaderSkipSamplesOffset, &header.skip_samples, sizeof(uint16_t));
    // Set original input sample rate in Hz.
    memcpy(output + kOpusHeaderSampleRateOffset, &input_sample_rate, sizeof(uint32_t));
    // Set output gain in dB.
    memcpy(output + kOpusHeaderGainOffset, &header.gain_db, sizeof(uint16_t));

    if (header.channels > 2) {
        // Set channel mapping
        output[kOpusHeaderChannelMappingOffset] = 1;
        // Assuming no coupled streams. This should actually be
        // channels() - |coupled_streams|.
        output[kOpusHeaderNumStreamsOffset] = header.channels;
        output[kOpusHeaderNumCoupledStreamsOffset] = 0;

        // Set the actual stream map.
        for (int i = 0; i < header.channels; ++i) {
            output[kOpusHeaderStreamMapOffset + i] = kOpusChannelMap[header.channels - 1][i];
        }
        return kOpusHeaderStreamMapOffset + header.channels + 1;
    } else {
        output[kOpusHeaderChannelMappingOffset] = 0;
        return kOpusHeaderChannelMappingOffset + 1;
    }
}

}  // namespace android
