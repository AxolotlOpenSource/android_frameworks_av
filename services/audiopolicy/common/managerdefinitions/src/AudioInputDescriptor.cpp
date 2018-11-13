/*
 * Copyright (C) 2015 The Android Open Source Project
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

#define LOG_TAG "APM::AudioInputDescriptor"
//#define LOG_NDEBUG 0

#include <media/AudioPolicy.h>
#include <policy.h>
#include <AudioPolicyInterface.h>
#include "AudioInputDescriptor.h"
#include "IOProfile.h"
#include "AudioGain.h"
#include "HwModule.h"

namespace android {

AudioInputDescriptor::AudioInputDescriptor(const sp<IOProfile>& profile,
                                           AudioPolicyClientInterface *clientInterface)
    : mProfile(profile)
    ,  mClientInterface(clientInterface)
{
    if (profile != NULL) {
        profile->pickAudioProfile(mSamplingRate, mChannelMask, mFormat);
        if (profile->mGains.size() > 0) {
            profile->mGains[0]->getDefaultConfig(&mGain);
        }
    }
}

audio_module_handle_t AudioInputDescriptor::getModuleHandle() const
{
    if (mProfile == 0) {
        return AUDIO_MODULE_HANDLE_NONE;
    }
    return mProfile->getModuleHandle();
}

audio_port_handle_t AudioInputDescriptor::getId() const
{
    return mId;
}

audio_source_t AudioInputDescriptor::inputSource(bool activeOnly) const
{
    return getHighestPrioritySource(activeOnly);
}

void AudioInputDescriptor::toAudioPortConfig(struct audio_port_config *dstConfig,
                                             const struct audio_port_config *srcConfig) const
{
    ALOG_ASSERT(mProfile != 0,
                "toAudioPortConfig() called on input with null profile %d", mIoHandle);
    dstConfig->config_mask = AUDIO_PORT_CONFIG_SAMPLE_RATE|AUDIO_PORT_CONFIG_CHANNEL_MASK|
                            AUDIO_PORT_CONFIG_FORMAT|AUDIO_PORT_CONFIG_GAIN;
    if (srcConfig != NULL) {
        dstConfig->config_mask |= srcConfig->config_mask;
    }

    AudioPortConfig::toAudioPortConfig(dstConfig, srcConfig);

    dstConfig->id = mId;
    dstConfig->role = AUDIO_PORT_ROLE_SINK;
    dstConfig->type = AUDIO_PORT_TYPE_MIX;
    dstConfig->ext.mix.hw_module = getModuleHandle();
    dstConfig->ext.mix.handle = mIoHandle;
    dstConfig->ext.mix.usecase.source = inputSource();
}

void AudioInputDescriptor::toAudioPort(struct audio_port *port) const
{
    ALOG_ASSERT(mProfile != 0, "toAudioPort() called on input with null profile %d", mIoHandle);

    mProfile->toAudioPort(port);
    port->id = mId;
    toAudioPortConfig(&port->active_config);
    port->ext.mix.hw_module = getModuleHandle();
    port->ext.mix.handle = mIoHandle;
    port->ext.mix.latency_class = AUDIO_LATENCY_NORMAL;
}

void AudioInputDescriptor::setPreemptedSessions(const SortedVector<audio_session_t>& sessions)
{
    mPreemptedSessions = sessions;
}

SortedVector<audio_session_t> AudioInputDescriptor::getPreemptedSessions() const
{
    return mPreemptedSessions;
}

bool AudioInputDescriptor::hasPreemptedSession(audio_session_t session) const
{
    return (mPreemptedSessions.indexOf(session) >= 0);
}

void AudioInputDescriptor::clearPreemptedSessions()
{
    mPreemptedSessions.clear();
}

bool AudioInputDescriptor::isSourceActive(audio_source_t source) const
{
    for (const auto &client : getClientIterable()) {
        if (client->active() &&
            ((client->source() == source) ||
                ((source == AUDIO_SOURCE_VOICE_RECOGNITION) &&
                    (client->source() == AUDIO_SOURCE_HOTWORD) &&
                    client->isSoundTrigger()))) {
            return true;
        }
    }
    return false;
}

audio_source_t AudioInputDescriptor::getHighestPrioritySource(bool activeOnly) const
{
    audio_source_t source = AUDIO_SOURCE_DEFAULT;
    int32_t priority = -1;

    for (const auto &client : getClientIterable()) {
        if (activeOnly && !client->active() ) {
            continue;
        }
        int32_t curPriority = source_priority(client->source());
        if (curPriority > priority) {
            priority = curPriority;
            source = client->source();
        }
    }
    return source;
}

bool AudioInputDescriptor::isSoundTrigger() const {
    // sound trigger and non sound trigger clients are not mixed on a given input
    // so check only first client
    if (getClientCount() == 0) {
        return false;
    }
    return getClientIterable().begin()->isSoundTrigger();
}

audio_patch_handle_t AudioInputDescriptor::getPatchHandle() const
{
    return mPatchHandle;
}

void AudioInputDescriptor::setPatchHandle(audio_patch_handle_t handle)
{
    mPatchHandle = handle;
    for (const auto &client : getClientIterable()) {
        if (client->active()) {
            updateClientRecordingConfiguration(RECORD_CONFIG_EVENT_START, client);
        }
    }
}

audio_config_base_t AudioInputDescriptor::getConfig() const
{
    const audio_config_base_t config = { .sample_rate = mSamplingRate, .channel_mask = mChannelMask,
            .format = mFormat };
    return config;
}

status_t AudioInputDescriptor::open(const audio_config_t *config,
                                       audio_devices_t device,
                                       const String8& address,
                                       audio_source_t source,
                                       audio_input_flags_t flags,
                                       audio_io_handle_t *input)
{
    audio_config_t lConfig;
    if (config == nullptr) {
        lConfig = AUDIO_CONFIG_INITIALIZER;
        lConfig.sample_rate = mSamplingRate;
        lConfig.channel_mask = mChannelMask;
        lConfig.format = mFormat;
    } else {
        lConfig = *config;
    }

    mDevice = device;

    ALOGV("opening input for device %08x address %s profile %p name %s",
          mDevice, address.string(), mProfile.get(), mProfile->getName().string());

    status_t status = mClientInterface->openInput(mProfile->getModuleHandle(),
                                                  input,
                                                  &lConfig,
                                                  &mDevice,
                                                  address,
                                                  source,
                                                  flags);
    LOG_ALWAYS_FATAL_IF(mDevice != device,
                        "%s openInput returned device %08x when given device %08x",
                        __FUNCTION__, mDevice, device);

    if (status == NO_ERROR) {
        LOG_ALWAYS_FATAL_IF(*input == AUDIO_IO_HANDLE_NONE,
                            "%s openInput returned input handle %d for device %08x",
                            __FUNCTION__, *input, device);
        mSamplingRate = lConfig.sample_rate;
        mChannelMask = lConfig.channel_mask;
        mFormat = lConfig.format;
        mId = AudioPort::getNextUniqueId();
        mIoHandle = *input;
        mProfile->curOpenCount++;
    }

    return status;
}

status_t AudioInputDescriptor::start()
{
    if (mGlobalActiveCount == 1) {
        if (!mProfile->canStartNewIo()) {
            ALOGI("%s mProfile->curActiveCount %d", __func__, mProfile->curActiveCount);
            return INVALID_OPERATION;
        }
        mProfile->curActiveCount++;
    }
    return NO_ERROR;
}

void AudioInputDescriptor::stop()
{
    if (!isActive()) {
        LOG_ALWAYS_FATAL_IF(mProfile->curActiveCount < 1,
                            "%s invalid profile active count %u",
                            __func__, mProfile->curActiveCount);
        mProfile->curActiveCount--;
    }
}

void AudioInputDescriptor::close()
{
    if (mIoHandle != AUDIO_IO_HANDLE_NONE) {
        mClientInterface->closeInput(mIoHandle);
        LOG_ALWAYS_FATAL_IF(mProfile->curOpenCount < 1, "%s profile open count %u",
                            __FUNCTION__, mProfile->curOpenCount);
        // do not call stop() here as stop() is supposed to be called after
        //  setClientActive(client, false) and we don't know how many clients
        // are still active at this time
        if (isActive()) {
            mProfile->curActiveCount--;
        }
        mProfile->curOpenCount--;
        LOG_ALWAYS_FATAL_IF(mProfile->curOpenCount <  mProfile->curActiveCount,
                "%s(%d): mProfile->curOpenCount %d < mProfile->curActiveCount %d.",
                __func__, mId, mProfile->curOpenCount, mProfile->curActiveCount);
        mIoHandle = AUDIO_IO_HANDLE_NONE;
    }
}

void AudioInputDescriptor::setClientActive(const sp<RecordClientDescriptor>& client, bool active)
{
    LOG_ALWAYS_FATAL_IF(getClient(client->portId()) == nullptr,
        "%s(%d) does not exist on input descriptor", __func__, client->portId());
    if (active == client->active()) {
        return;
    }

    // Handle non-client-specific activity ref count
    int32_t oldGlobalActiveCount = mGlobalActiveCount;
    if (!active && mGlobalActiveCount < 1) {
        LOG_ALWAYS_FATAL("%s(%d) invalid deactivation with globalActiveCount %d",
               __func__, client->portId(), mGlobalActiveCount);
        // mGlobalActiveCount = 1;
    }
    const int delta = active ? 1 : -1;
    mGlobalActiveCount += delta;

    if ((oldGlobalActiveCount == 0) && (mGlobalActiveCount > 0)) {
        if ((mPolicyMix != NULL) && ((mPolicyMix->mCbFlags & AudioMix::kCbFlagNotifyActivity) != 0))
        {
            mClientInterface->onDynamicPolicyMixStateUpdate(mPolicyMix->mDeviceAddress,
                                                            MIX_STATE_MIXING);
        }
    } else if ((oldGlobalActiveCount > 0) && (mGlobalActiveCount == 0)) {
        if ((mPolicyMix != NULL) && ((mPolicyMix->mCbFlags & AudioMix::kCbFlagNotifyActivity) != 0))
        {
            mClientInterface->onDynamicPolicyMixStateUpdate(mPolicyMix->mDeviceAddress,
                                                            MIX_STATE_IDLE);
        }
    }

    client->setActive(active);

    int event = active ? RECORD_CONFIG_EVENT_START : RECORD_CONFIG_EVENT_STOP;
    updateClientRecordingConfiguration(event, client);

}

void AudioInputDescriptor::updateClientRecordingConfiguration(
    int event, const sp<RecordClientDescriptor>& client)
{
    const audio_config_base_t sessionConfig = client->config();
    const record_client_info_t recordClientInfo{client->uid(), client->session(), client->source()};
    const audio_config_base_t config = getConfig();
    mClientInterface->onRecordingConfigurationUpdate(event,
                                                     &recordClientInfo, &sessionConfig,
                                                     &config, mPatchHandle);
}

RecordClientVector AudioInputDescriptor::getClientsForSession(
    audio_session_t session)
{
    RecordClientVector clients;
    for (const auto &client : getClientIterable()) {
        if (client->session() == session) {
            clients.push_back(client);
        }
    }
    return clients;
}

RecordClientVector AudioInputDescriptor::clientsList(bool activeOnly, audio_source_t source,
                                                     bool preferredDeviceOnly) const
{
    RecordClientVector clients;
    for (const auto &client : getClientIterable()) {
        if ((!activeOnly || client->active())
            && (source == AUDIO_SOURCE_DEFAULT || source == client->source())
            && (!preferredDeviceOnly || client->hasPreferredDevice())) {
            clients.push_back(client);
        }
    }
    return clients;
}

void AudioInputDescriptor::dump(String8 *dst) const
{
    dst->appendFormat(" ID: %d\n", getId());
    dst->appendFormat(" Sampling rate: %d\n", mSamplingRate);
    dst->appendFormat(" Format: %d\n", mFormat);
    dst->appendFormat(" Channels: %08x\n", mChannelMask);
    dst->appendFormat(" Devices %08x\n", mDevice);
    dst->append(" AudioRecord Clients:\n");
    ClientMapHandler<RecordClientDescriptor>::dump(dst);
    dst->append("\n");
}

bool AudioInputCollection::isSourceActive(audio_source_t source) const
{
    for (size_t i = 0; i < size(); i++) {
        const sp<AudioInputDescriptor>  inputDescriptor = valueAt(i);
        if (inputDescriptor->isSourceActive(source)) {
            return true;
        }
    }
    return false;
}

sp<AudioInputDescriptor> AudioInputCollection::getInputFromId(audio_port_handle_t id) const
{
    for (size_t i = 0; i < size(); i++) {
        const sp<AudioInputDescriptor> inputDescriptor = valueAt(i);
        if (inputDescriptor->getId() == id) {
            return inputDescriptor;
        }
    }
    return NULL;
}

uint32_t AudioInputCollection::activeInputsCountOnDevices(audio_devices_t devices) const
{
    uint32_t count = 0;
    for (size_t i = 0; i < size(); i++) {
        const sp<AudioInputDescriptor>  inputDescriptor = valueAt(i);
        if (inputDescriptor->isActive() &&
                ((devices == AUDIO_DEVICE_IN_DEFAULT) ||
                 ((inputDescriptor->mDevice & devices & ~AUDIO_DEVICE_BIT_IN) != 0))) {
            count++;
        }
    }
    return count;
}

Vector<sp <AudioInputDescriptor> > AudioInputCollection::getActiveInputs(bool ignoreVirtualInputs)
{
    Vector<sp <AudioInputDescriptor> > activeInputs;

    for (size_t i = 0; i < size(); i++) {
        const sp<AudioInputDescriptor>  inputDescriptor = valueAt(i);
        if ((inputDescriptor->isActive())
                && (!ignoreVirtualInputs ||
                    !is_virtual_input_device(inputDescriptor->mDevice))) {
            activeInputs.add(inputDescriptor);
        }
    }
    return activeInputs;
}

audio_devices_t AudioInputCollection::getSupportedDevices(audio_io_handle_t handle) const
{
    sp<AudioInputDescriptor> inputDesc = valueFor(handle);
    audio_devices_t devices = inputDesc->mProfile->getSupportedDevicesType();
    return devices;
}

sp<AudioInputDescriptor> AudioInputCollection::getInputForClient(audio_port_handle_t portId)
{
    for (size_t i = 0; i < size(); i++) {
        sp<AudioInputDescriptor> inputDesc = valueAt(i);
        if (inputDesc->getClient(portId) != nullptr) {
            return inputDesc;
        }
    }
    return 0;
}

void AudioInputCollection::dump(String8 *dst) const
{
    dst->append("\nInputs dump:\n");
    for (size_t i = 0; i < size(); i++) {
        dst->appendFormat("- Input %d dump:\n", keyAt(i));
        valueAt(i)->dump(dst);
    }
}

}; //namespace android
