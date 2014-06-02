#
# Copyright (C) 2011 The Android Open-Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# define OMAP_ENHANCEMENT variables
include device/phytec/pcm049/Config.mk

PRODUCT_COPY_FILES := \
	device/phytec/pcm049/init.pcm049.rc:root/init.pcm049.rc \
	device/phytec/pcm049/init.pcm049.usb.rc:root/init.pcm049.usb.rc \
	device/phytec/pcm049/init.pcm049.wifi.rc:root/init.pcm049.wifi.rc \
        device/phytec/pcm049/fstab.pcm049:root/fstab.pcm049 \
	device/phytec/pcm049/ueventd.pcm049.rc:root/ueventd.pcm049.rc \
	device/phytec/pcm049/mixer_paths.xml:system/etc/mixer_paths.xml \
	device/phytec/pcm049/media_profiles.xml:system/etc/media_profiles.xml \
	device/phytec/pcm049/media_codecs.xml:system/etc/media_codecs.xml \
	frameworks/native/data/etc/android.hardware.usb.host.xml:system/etc/permissions/android.hardware.usb.host.xml \
	frameworks/native/data/etc/android.hardware.wifi.xml:system/etc/permissions/android.hardware.wifi.xml \
	frameworks/native/data/etc/android.hardware.wifi.direct.xml:system/etc/permissions/android.hardware.wifi.direct.xml \
	frameworks/native/data/etc/android.hardware.usb.accessory.xml:system/etc/permissions/android.hardware.usb.accessory.xml \
	frameworks/native/data/etc/android.hardware.camera.xml:system/etc/permissions/android.hardware.camera.xml \
	frameworks/native/data/etc/android.hardware.sensor.accelerometer.xml:system/etc/permissions/android.hardware.sensor.accelerometer.xml \
	device/phytec/pcm049/android.hardware.ethernet.xml:system/etc/permissions/android.hardware.ethernet.xml \
	device/phytec/pcm049/android.hardware.bluetooth.xml:system/etc/permissions/android.hardware.bluetooth.xml \
        device/phytec/pcm049/android.hardware.location.gps.xml:system/etc/permissions/android.hardware.location.gps.xml \
	device/phytec/pcm049/omap4-keypad.kl:system/usr/keylayout/omap4-keypad.kl \
	device/phytec/pcm049/omap4-keypad.kcm:system/usr/keychars/omap4-keypad.kcm \
        device/phytec/pcm049/ft5x06_ts.idc:system/usr/idc/ft5x06_ts.idc \
        device/common/gps/gps.conf_US:system/etc/gps.conf \
	device/phytec/pcm049/audio/audio_policy.conf:system/etc/audio_policy.conf \
        #device/phytec/pcm049/twl6030_pwrbutton.kl:system/usr/keylayout/twl6030_pwrbutton.kl \

# to mount the external storage (sdcard)
PRODUCT_COPY_FILES += \
        device/phytec/pcm049/vold.fstab:system/etc/vold.fstab

PRODUCT_PACKAGES += \
	lights.pcm049

#Sensors - Accelerometer
PRODUCT_PACKAGES += \
	sensors.pcm049 \
	sensor.test \

# Live Wallpapers
PRODUCT_PACKAGES += \
        LiveWallpapers \
        LiveWallpapersPicker \
        MagicSmokeWallpapers \
        VisualizationWallpapers \
        librs_jni \

PRODUCT_PACKAGES += \
    ti_omap4_ducati_bins
#    libOMX_Core \
#    libOMX.TI.DUCATI1.VIDEO.DECODER

# Tiler
PRODUCT_PACKAGES += \
    libtimemmgr

# Camera
ifdef OMAP_ENHANCEMENT_CPCAM
PRODUCT_PACKAGES += \
    libcpcam_jni \
    com.ti.omap.android.cpcam

PRODUCT_COPY_FILES += \
    hardware/ti/omap4xxx/cpcam/com.ti.omap.android.cpcam.xml:system/etc/permissions/com.ti.omap.android.cpcam.xml
endif

PRODUCT_PACKAGES += \
    CameraOMAP \
    Camera \
    camera_test

PRODUCT_PROPERTY_OVERRIDES := \
        hwui.render_dirty_regions=false

PRODUCT_DEFAULT_PROPERTY_OVERRIDES += \
        persist.sys.usb.config=mtp

PRODUCT_PROPERTY_OVERRIDES += \
        ro.opengles.version=131072

PRODUCT_PROPERTY_OVERRIDES += \
        ro.sf.lcd_density=240

PRODUCT_PACKAGES += \
        make_ext4fs \
	com.android.future.usb.accessory

PRODUCT_PROPERTY_OVERRIDES += \
	wifi.interface=wlan0 \
	hwui.render_dirty_regions=false

PRODUCT_CHARACTERISTICS := tablet,nosdcard

DEVICE_PACKAGE_OVERLAYS := \
    device/phytec/pcm049/overlay

#HWC Hal
PRODUCT_PACKAGES += \
    hwcomposer.omap4

PRODUCT_TAGS += dalvik.gc.type-precise

PRODUCT_PACKAGES += \
	librs_jni \
	com.android.future.usb.accessory

PRODUCT_PACKAGES += \
	audio.primary.pcm049

# tinyalsa utils
PRODUCT_PACKAGES += \
	tinymix \
	tinyplay \
	tinycap

# Audioout libs
PRODUCT_PACKAGES += \
	libaudioutils

# Enable AAC 5.1 decode (decoder)
PRODUCT_PROPERTY_OVERRIDES += \
	media.aac_51_output_enabled=true

PRODUCT_PACKAGES += \
	dhcpcd.conf \
	TQS_D_1.7.ini \
	TQS_D_1.7_127x.ini \
	hostapd.conf \
	wifical.sh \
	wilink7.sh \
	calibrator

# Filesystem management tools
PRODUCT_PACKAGES += \
	make_ext4fs

# to flow down to ti-wpan-products.mk
BLUETI_ENHANCEMENT := true

# BlueZ a2dp Audio HAL module
#PRODUCT_PACKAGES += audio.a2dp.default

# BlueZ test tools
PRODUCT_PACKAGES += \
	hciconfig \
	hcitool

DUCATI_TGZ := device/ti/proprietary-open/omap4/ducati_blaze_tablet.tgz
PRODUCT_PACKAGES += ducati-m3-core0.xem3

$(call inherit-product-if-exists, vendor/sciaps/device.mk)
$(call inherit-product, frameworks/native/build/tablet-dalvik-heap.mk)
$(call inherit-product, hardware/ti/omap4xxx/omap4.mk)
$(call inherit-product-if-exists, device/ti/proprietary-open/omap4/ti-omap4-vendor.mk)
$(call inherit-product-if-exists, vendor/ti/proprietary/omap4/ti-omap4-vendor.mk)
$(call inherit-product-if-exists, hardware/ti/wpan/ti-wpan-products.mk)
$(call inherit-product, device/phytec/pcm049/wl12xx/ti-wl12xx-vendor.mk)
$(call inherit-product-if-exists, device/ti/proprietary-open/wl12xx/wlan/wl12xx-wlan-fw-products.mk)
#$(call inherit-product-if-exists, device/ti/common-open/s3d/s3d-products.mk)
#$(call inherit-product-if-exists, device/ti/proprietary-open/omap4/ducati-full_blaze.mk)
#$(call inherit-product-if-exists, device/ti/proprietary-open/omap4/dsp_fw.mk)
$(call inherit-product-if-exists, hardware/ti/dvp/dvp-products.mk)
$(call inherit-product-if-exists, hardware/ti/arx/arx-products.mk)
