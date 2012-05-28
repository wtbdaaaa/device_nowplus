/*
 * Copyright (C) 2008 The Android Open Source Project
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

#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/select.h>

#include <cutils/log.h>

#include "ProximitySensor.h"

/*****************************************************************************/

ProximitySensor::ProximitySensor()
    : SensorBase(CM_DEVICE_NAME, "proximity sensor"),
      mEnabled(0),
      mInputReader(4),
      mHasPendingEvent(false)
{
    mPendingEvent.version = sizeof(sensors_event_t);
    mPendingEvent.sensor = ID_P;
    mPendingEvent.type = SENSOR_TYPE_PROXIMITY;
    memset(mPendingEvent.data, 0, sizeof(mPendingEvent.data));

 

        enable(0, 1);


}

ProximitySensor::~ProximitySensor() {
    if (mEnabled) {
        enable(0, 0);
    }
}

int ProximitySensor::setInitialState() {
    struct input_absinfo absinfo;
    if (!ioctl(data_fd, EVIOCGABS(EVENT_TYPE_PROXIMITY), &absinfo)) {
        // make sure to report an event immediately
        mHasPendingEvent = true;
        mPendingEvent.distance = indexToValue(absinfo.value);
    }
    return 0;
}

int ProximitySensor::enable(int32_t, int en) {
    int newState = en ? 1 : 0;
    int err = 0;
    if (newState != mEnabled) {
        if (!mEnabled) {
            open_device();
        }
            mEnabled = newState;
            if (en) {
                uint16_t mode = 1;
                err = ioctl(dev_fd, P_IOC_POWERUP_SET_MODE, &mode);
        if (err < 0)
            LOGE("P_IOC_POWERUP_SET_MODE error (%s)", strerror(errno));
         
                setInitialState();
           }
           else {
        err = ioctl(dev_fd, P_IOC_SHUTDOWN);
        if (err < 0)
            LOGE("P_IOC_POWERUP_SET_MODE error (%s)", strerror(errno));
        }

        if (!mEnabled) {
            close_device();
        }
    }
    return err;
}

bool ProximitySensor::hasPendingEvents() const {
    return mHasPendingEvent;
}

int ProximitySensor::readEvents(sensors_event_t* data, int count)
{
    if (count < 1)
        return -EINVAL;

    if (mHasPendingEvent) {
        mHasPendingEvent = false;
        mPendingEvent.timestamp = getTimestamp();
        *data = mPendingEvent;
        return mEnabled ? 1 : 0;
    }

    ssize_t n = mInputReader.fill(data_fd);
    if (n < 0)
        return n;

    int numEventReceived = 0;
    input_event const* event;

    while (count && mInputReader.readEvent(&event)) {
        int type = event->type;
        if (type == EV_ABS) {
            if (event->code == EVENT_TYPE_PROXIMITY) {
                mPendingEvent.distance = indexToValue(event->value);
            }
        } else if (type == EV_SYN) {
            mPendingEvent.timestamp = timevalToNano(event->time);
            if (mEnabled) {
                *data++ = mPendingEvent;
                count--;
                numEventReceived++;
            }
        } else {
            LOGE("ProximitySensor: unknown event (type=%d, code=%d)",
                    type, event->code);
        }
        mInputReader.next();
    }

    return numEventReceived;
}

float ProximitySensor::indexToValue(size_t index) const
{
    //return (index * 6/180)-1;
    return index * PROXIMITY_THRESHOLD_GP2A;
}
