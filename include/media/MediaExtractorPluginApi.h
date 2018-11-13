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

#ifndef MEDIA_EXTRACTOR_PLUGIN_API_H_
#define MEDIA_EXTRACTOR_PLUGIN_API_H_

#include <utils/Errors.h> // for status_t

struct AMediaFormat;

namespace android {

struct MediaTrack;
class MetaDataBase;
class MediaBufferBase;

extern "C" {

struct CDataSource {
    ssize_t (*readAt)(void *handle, off64_t offset, void *data, size_t size);
    status_t (*getSize)(void *handle, off64_t *size);
    uint32_t (*flags)(void *handle );
    bool (*getUri)(void *handle, char *uriString, size_t bufferSize);
    void *handle;
};

enum CMediaTrackReadOptions : uint32_t {
    SEEK_PREVIOUS_SYNC = 0,
    SEEK_NEXT_SYNC = 1,
    SEEK_CLOSEST_SYNC = 2,
    SEEK_CLOSEST = 3,
    SEEK_FRAME_INDEX = 4,
    SEEK = 8,
    NONBLOCKING = 16
};

struct CMediaTrack {
    void *data;
    void (*free)(void *data);

    status_t (*start)(void *data);
    status_t (*stop)(void *data);
    status_t (*getFormat)(void *data, MetaDataBase &format);
    status_t (*read)(void *data, MediaBufferBase **buffer, uint32_t options, int64_t seekPosUs);
    bool     (*supportsNonBlockingRead)(void *data);
};

struct CMediaTrackV2 {
    void *data;
    void (*free)(void *data);

    status_t (*start)(void *data);
    status_t (*stop)(void *data);
    status_t (*getFormat)(void *data, AMediaFormat *format);
    status_t (*read)(void *data, MediaBufferBase **buffer, uint32_t options, int64_t seekPosUs);
    bool     (*supportsNonBlockingRead)(void *data);
};


struct CMediaExtractorV1 {
    void *data;

    void (*free)(void *data);
    size_t (*countTracks)(void *data);
    CMediaTrack* (*getTrack)(void *data, size_t index);
    status_t (*getTrackMetaData)(
            void *data,
            MetaDataBase& meta,
            size_t index, uint32_t flags);

    status_t (*getMetaData)(void *data, MetaDataBase& meta);
    uint32_t (*flags)(void *data);
    status_t (*setMediaCas)(void *data, const uint8_t* casToken, size_t size);
    const char * (*name)(void *data);
};

struct CMediaExtractorV2 {
    void *data;

    void (*free)(void *data);
    size_t (*countTracks)(void *data);
    CMediaTrackV2* (*getTrack)(void *data, size_t index);
    status_t (*getTrackMetaData)(
            void *data,
            AMediaFormat *meta,
            size_t index, uint32_t flags);

    status_t (*getMetaData)(void *data, AMediaFormat *meta);
    uint32_t (*flags)(void *data);
    status_t (*setMediaCas)(void *data, const uint8_t* casToken, size_t size);
    const char * (*name)(void *data);
};

typedef CMediaExtractorV1* (*CreatorFuncV1)(CDataSource *source, void *meta);
typedef void (*FreeMetaFunc)(void *meta);

// The sniffer can optionally fill in an opaque object, "meta", that helps
// the corresponding extractor initialize its state without duplicating
// effort already exerted by the sniffer. If "freeMeta" is given, it will be
// called against the opaque object when it is no longer used.
typedef CreatorFuncV1 (*SnifferFuncV1)(
        CDataSource *source, float *confidence,
        void **meta, FreeMetaFunc *freeMeta);

typedef CMediaExtractorV2* (*CreatorFuncV2)(CDataSource *source, void *meta);

typedef CreatorFuncV2 (*SnifferFuncV2)(
        CDataSource *source, float *confidence,
        void **meta, FreeMetaFunc *freeMeta);

typedef CMediaExtractorV1 CMediaExtractor;
typedef CreatorFuncV1 CreatorFunc;


typedef struct {
    const uint8_t b[16];
} media_uuid_t;

struct ExtractorDef {
    // version number of this structure
    const uint32_t def_version;

    // A unique identifier for this extractor.
    // See below for a convenience macro to create this from a string.
    media_uuid_t extractor_uuid;

    // Version number of this extractor. When two extractors with the same
    // uuid are encountered, the one with the largest version number will
    // be used.
    const uint32_t extractor_version;

    // a human readable name
    const char *extractor_name;

    union {
        SnifferFuncV1 v1;
        SnifferFuncV2 v2;
    } sniff;
};

const uint32_t EXTRACTORDEF_VERSION_LEGACY = 1;
const uint32_t EXTRACTORDEF_VERSION_CURRENT = 2;

const uint32_t EXTRACTORDEF_VERSION = EXTRACTORDEF_VERSION_LEGACY;

// each plugin library exports one function of this type
typedef ExtractorDef (*GetExtractorDef)();

} // extern "C"

}  // namespace android

#endif  // MEDIA_EXTRACTOR_PLUGIN_API_H_
