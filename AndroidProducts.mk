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

PRODUCT_MAKEFILES := $(LOCAL_DIR)/full_pcm049.mk

ifdef OMAP_ENHANCEMENT_CPCAM
PRODUCT_MAKEFILES += \
    device/ti/blaze_tablet/sdk_addon/ti_omap_addon.mk
endif

# clear OMAP_ENHANCEMENT variables
$(call ti-clear-vars)
