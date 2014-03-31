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

#include "ADXL34xSensor.h"

#define ACCEL_SENSOR_NAME "ADXL34x accelerometer"
#define ADXL_MAX_SAMPLE_RATE_VAL 11 /* 200 Hz */

#define SEC_TO_NSEC		1000000000LL
#define USEC_TO_NSEC		1000
#define MSEC_TO_USEC		1000

#define ID_ACCELERATION		0
#define ACCELERATION_X          (1 << ABS_X)
#define ACCELERATION_Y          (1 << ABS_Y)
#define ACCELERATION_Z          (1 << ABS_Z)

#define FETCH_FULL_EVENT_BEFORE_RETURN 1

/*****************************************************************************/

ADXL34xSensor::ADXL34xSensor()
    : SensorBase(NULL, ACCEL_SENSOR_NAME),
      mEnabled(0),
      mInputReader(4),
      mHasPendingEvent(false)
{
    mPendingEvent.version = sizeof(sensors_event_t);
    mPendingEvent.sensor = ID_A;
    mPendingEvent.type = SENSOR_TYPE_ACCELEROMETER;
    memset(mPendingEvent.data, 0, sizeof(mPendingEvent.data));

    if (data_fd) {
        strcpy(input_sysfs_path, "/sys/bus/i2c/drivers/adxl34x/4-0053/");
        input_sysfs_path_len = strlen(input_sysfs_path);
        enable(0, 1);
    }
}

ADXL34xSensor::~ADXL34xSensor() {
    if (mEnabled) {
        enable(0, 0);
    }
}

int ADXL34xSensor::setInitialState() {
    struct input_absinfo absinfo_x;
    struct input_absinfo absinfo_y;
    struct input_absinfo absinfo_z;
    float value;

    if (!ioctl(data_fd, EVIOCGABS(ABS_X), &absinfo_x) &&
        !ioctl(data_fd, EVIOCGABS(ABS_Y), &absinfo_y) &&
        !ioctl(data_fd, EVIOCGABS(ABS_Z), &absinfo_z)) {
        value = absinfo_x.value;
        mPendingEvent.acceleration.x = value * CONVERT_A_X;
        value = absinfo_y.value;
        mPendingEvent.acceleration.y = value * CONVERT_A_Y;
        value = absinfo_z.value;
        mPendingEvent.acceleration.z = value * CONVERT_A_Z;
        mHasPendingEvent = true;
    }
    return 0;
}

int ADXL34xSensor::enable(int32_t handle, int en) {
    /* handle check */
    if (handle != ID_A){
	ALOGE("ADXL34xSensor: Invalid handle (%d)", handle);
    }

    int flags = en ? 1 : 0;
    if (flags != mEnabled) {
        int fd;
        strcpy(&input_sysfs_path[input_sysfs_path_len], "disable");
        fd = open(input_sysfs_path, O_RDWR);
        if (fd >= 0) {
            char buf[1];
            int err;
            if (flags) {
                buf[0] = '0';
            } else {
                buf[0] = '1';
            }
            err = write(fd, buf, sizeof(buf));
            close(fd);
            mEnabled = flags;
            setInitialState();
            return 0;
        }
        return -1;
    }
    return 0;
}

bool ADXL34xSensor::hasPendingEvents() const {
    return mHasPendingEvent;
}

int ADXL34xSensor::setDelay(int32_t handle, int64_t delay_ns)
{
    int fd;
    int rate_val;
    int32_t us = (int32_t) (delay_ns / USEC_TO_NSEC);

    /* handle check */
    if (handle != ID_A){
        ALOGE("ADXL34xSensor: Invalid handle (%d)", handle);
    }
    strcpy(&input_sysfs_path[input_sysfs_path_len], "rate");
    fd = open(input_sysfs_path, O_RDWR);
    if (fd >= 0) {
        char buf[80];
	/*
	* The ADXL34x Supports 16 sample rates ranging from 3200Hz-0.098Hz
	* Calculate best fit and limit to max 200Hz (rate_val 11)
	*/
	for (rate_val = 0; rate_val < 16; rate_val++){
	    if (us  >= ((10000 * MSEC_TO_USEC) >> rate_val)){
		break;
	    }
	}
	if (rate_val > ADXL_MAX_SAMPLE_RATE_VAL) {
	    rate_val = ADXL_MAX_SAMPLE_RATE_VAL;
	}
	sprintf(buf, "%d", rate_val);
	write(fd, buf, strlen(buf)+1);
	close(fd);
        return 0;
    }
    return -1;
}

int ADXL34xSensor::readEvents(sensors_event_t* data, int count)
{
    int32_t new_sensors = 0;
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
            float value = event->value;
            if (event->code == ABS_X) {
		new_sensors |= ACCELERATION_X;
                mPendingEvent.acceleration.x = value * CONVERT_A_X;
            } else if (event->code == ABS_Y) {
		new_sensors |= ACCELERATION_Y;
                mPendingEvent.acceleration.y = value * CONVERT_A_Y;
            } else if (event->code == ABS_Z) {
		new_sensors |= ACCELERATION_Z;
                mPendingEvent.acceleration.z = value * CONVERT_A_Z;
            }
        } else if (type == EV_SYN) {
	    if (event->code == SYN_CONFIG) {
		/* Injected event by control_wake
		* return immediately
		*/
		return 0x7FFFFFFF;
	    }
	    /*
	     * Linux Input suppresses identical events, so if
	     * only ABS_Z changes and ABS_X,Y stays constant
	     * between events we need to report the cached
	     * Many other drivers start to scramble events by
	     * waiting for a full triplet to arrive.
	     * Other events on the ADXL345/6 such as TAP
	     * should be turned off.
	     */
	    if (new_sensors) {
		int64_t t = event->time.tv_sec * SEC_TO_NSEC + event->time.tv_usec * USEC_TO_NSEC;
		new_sensors = 0;

		mPendingEvent.timestamp = t;
		mPendingEvent.sensor = ID_ACCELERATION;
		return ID_ACCELERATION;
	    }
	    mPendingEvent.timestamp = timevalToNano(event->time);
            if (mEnabled) {
                *data++ = mPendingEvent;
                count--;
                numEventReceived++;
            }
        } else {
            ALOGE("ADXL34xSensor: unknown event (type=%d, code=%d)",
                    type, event->code);
        }
        mInputReader.next();
    }

    return numEventReceived;
}
